#include "pdf/pdf_service.hpp"
#include "pdf/font_adapter.hpp"
#include "pdf/file_reader.hpp"
#include "platform/mapped_file.hpp"
#include "platform/clear_secret.hpp"
#include "pdf/layout.hpp"
#include "fpdfview.h"
#include "fpdf_text.h"
#include "fpdf_edit.h"
#include "fpdf_progressive.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <list>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace opt {
namespace {
static_assert(pdf_render_flags==(FPDF_ANNOT|FPDF_RENDER_LIMITEDIMAGECACHE));
using Clock=std::chrono::steady_clock;
struct Page {
    FPDF_PAGE page=nullptr;
    FPDF_TEXTPAGE text=nullptr;
    ~Page(){if(text)FPDFText_ClosePage(text);if(page)FPDF_ClosePage(page);}
};
RenderTransform transform(FPDF_PAGE page,int width,int height,int rotation) {
    PdfPoint origin,x,y;
    if(!FPDF_DeviceToPage(page,0,0,width,height,rotation,0,0,&origin.x,&origin.y)||
       !FPDF_DeviceToPage(page,0,0,width,height,rotation,width,0,&x.x,&x.y)||
       !FPDF_DeviceToPage(page,0,0,width,height,rotation,0,height,&y.x,&y.y))
        throw std::runtime_error("ページの座標変換を取得できません。");
    Affine2D inverse{(x.x-origin.x)/width,(x.y-origin.y)/width,
                     (y.x-origin.x)/height,(y.y-origin.y)/height,origin.x,origin.y};
    return {width,height,rotation,inverse.inverse(),inverse};
}
PdfPageInfo describe(FPDF_PAGE page) {
    const auto w=FPDF_GetPageWidthF(page),h=FPDF_GetPageHeightF(page);
    if(!std::isfinite(w)||!std::isfinite(h)||w<=0||h<=0)throw std::runtime_error("ページ寸法が不正です。");
    // Layout uses one stable normalized coordinate system, independent of zoom.
    const int normalized_height=static_cast<int>(std::clamp(1024.0*h/w,1.0,static_cast<double>(max_page_pixels)));
    return {w,h,transform(page,1024,normalized_height,0).pdf_to_page_px,1024.0,static_cast<double>(normalized_height)};
}
struct ExtractedPage {PdfPageInfo info;std::shared_ptr<const PageLayout> layout;};
struct TextExtraction {
    std::shared_ptr<Page> owner;
    PageIndex index;
    std::vector<GlyphRecord> glyphs;
    int cursor=0,count=0;
    FPDF_FONT previous_font=nullptr;
    std::uint32_t font_run=0;
    LayoutOptions options;
    std::optional<PdfSearchRequest> search;
    TextExtraction(std::shared_ptr<Page> page,PageIndex number,LayoutOptions limits,std::optional<PdfSearchRequest> request={})
        :owner(std::move(page)),index(number),options(limits),search(std::move(request)){
        if(!owner->text)owner->text=FPDFText_LoadPage(owner->page);
        if(!owner->text)throw std::runtime_error("このページの文字を抽出できません。");
        count=FPDFText_CountChars(owner->text);if(count<0)throw std::runtime_error("文字数を取得できません。");
        try{LayoutEngine(options).check_glyph_budget(static_cast<std::size_t>(count));}
        catch(...){FPDFText_ClosePage(owner->text);owner->text=nullptr;throw;}
        glyphs.reserve(static_cast<std::size_t>(count));
    }
    bool step(const std::atomic_bool& stopping,const std::atomic<DocumentEpoch>& wanted,DocumentEpoch epoch){
        auto& page=*owner;const auto deadline=Clock::now()+std::chrono::milliseconds(10);
        const auto slice_end=cursor+std::min(2048,count-cursor);
        for(;cursor<slice_end;++cursor){
        if(cursor%64==0){if(stopping.load()||wanted.load()!=epoch)throw LayoutCancelled();if(Clock::now()>=deadline)return false;}
        const auto i=cursor;
        GlyphRecord g;g.index=static_cast<PdfCharIndex>(i);
        g.unicode=static_cast<char32_t>(FPDFText_GetUnicode(page.text,i));
        g.valid_bounds=FPDFText_GetCharBox(page.text,i,&g.bounds.left,&g.bounds.right,&g.bounds.bottom,&g.bounds.top);
        FPDFText_GetCharOrigin(page.text,i,&g.origin.x,&g.origin.y);
        g.font_size=FPDFText_GetFontSize(page.text,i);g.angle_radians=FPDFText_GetCharAngle(page.text,i);
        // PDFium returns the text object's unscaled Tf size. Origins and ink
        // boxes already include Tm, cm and enclosing Form matrices. Put the em
        // height in that same page space before using it for line/word gaps.
        FS_MATRIX matrix{};
        if(FPDFText_GetMatrix(page.text,i,&matrix))
            g.font_size*=std::hypot(matrix.c,matrix.d);
        g.generated=FPDFText_IsGenerated(page.text,i)==1;g.unicode_map_error=FPDFText_HasUnicodeMapError(page.text,i)==1;
        // Layout compares adjacent characters' fonts. A run number preserves
        // that relation without materializing the document's font names.
        const auto font=FPDFTextObj_GetFont(FPDFText_GetTextObject(page.text,i));
        if(font!=previous_font){++font_run;previous_font=font;}
        g.font_id=font_run;
        glyphs.push_back(g);
        }
        return cursor==count;
    }
    ExtractedPage finish(const std::function<bool()>& cancelled){const auto info=describe(owner->page);return {info,std::make_shared<const PageLayout>(LayoutEngine(options).build(index,std::move(glyphs),info.pdf_to_layout,info.layout_width,info.layout_height,cancelled))};}
};
bool same_geometry(const TileKey& a,const ViewRequest& b) {
    return a.document==b.document&&a.page==b.page&&a.full_width==b.width&&
        a.full_height==b.height&&a.rotation==b.rotation;
}
struct Library {Library(){FPDF_LIBRARY_CONFIG c{};c.version=2;FPDF_InitLibraryWithConfig(&c);}~Library(){FPDF_DestroyLibrary();}};
}

struct PdfService::Impl {
    struct OpenRequest {
        DocumentEpoch epoch;
        std::filesystem::path path;
        std::string password;
        OpenRequest(DocumentEpoch e,const std::filesystem::path& p,std::string_view secret)
            :epoch(e),path(p),password(secret){}
        ~OpenRequest(){clear_secret(password);}
    };
    Delivery delivery;
    std::filesystem::path font_directory;
    LayoutOptions layout_options;
    std::mutex mutex;
    std::condition_variable cv;
    std::unique_ptr<OpenRequest> opening;
    std::optional<std::pair<DocumentEpoch,PageIndex>> requested_page;
    std::optional<ViewRequest> requested_view;
    std::optional<PdfSearchRequest> searching;
    std::deque<std::pair<DocumentEpoch,PageIndex>> extraction;
    std::optional<std::pair<DocumentEpoch,PageIndex>> extracting;
    std::atomic_bool stopping{false},finished{false};
    std::atomic<std::uint64_t> request_generation{0};
    std::atomic<DocumentEpoch> requested_document{0};
    std::atomic<PageIndex> displayed_page{0};
    std::atomic<std::uint64_t> desired_query{0};
    std::atomic<std::uint64_t> page_peak{0},queue_peak{0},tile_peak{0},render_slices{0},render_yields{0},text_slices{0},text_yields{0},render_max_us{0};
    static void maximum(std::atomic<std::uint64_t>& target,std::uint64_t value){auto prior=target.load();while(prior<value&&!target.compare_exchange_weak(prior,value)){}}
    void render_slice(Clock::time_point start,int status){++render_slices;if(status==FPDF_RENDER_TOBECONTINUED)++render_yields;maximum(render_max_us,std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count());}
    std::thread worker;
    explicit Impl(Delivery callback,std::filesystem::path fonts,LayoutOptions limits):delivery(std::move(callback)),font_directory(std::move(fonts)),layout_options(limits),worker([this]{run();}){}
    ~Impl(){stop();if(worker.joinable())worker.join();}
    void stop() noexcept{std::lock_guard lock(mutex);stopping=true;cv.notify_one();}
    void emit(PdfEvent event){delivery(std::make_shared<const PdfEvent>(std::move(event)));}

    struct ActiveRender : IFSDK_PAUSE {
        Impl* owner;
        std::uint64_t generation;
        Clock::time_point deadline;
        std::shared_ptr<Page> page;
        std::shared_ptr<RasterBuffer> pixels;
        FPDF_BITMAP bitmap=nullptr;
        TileKey key;
        RenderTransform mapping;
        int status=FPDF_RENDER_TOBECONTINUED;
        bool started=false;
        ActiveRender(Impl* service,std::uint64_t g):owner(service),generation(g){
            version=1;
            NeedToPauseNow=[](IFSDK_PAUSE* pause)->FPDF_BOOL {
                auto* p=static_cast<ActiveRender*>(pause);
                return p->owner->stopping.load()||p->owner->request_generation.load()!=p->generation||Clock::now()>=p->deadline;
            };
        }
        ~ActiveRender(){if(started)FPDF_RenderPage_Close(page->page);if(bitmap)FPDFBitmap_Destroy(bitmap);}
    };

    void run() noexcept {
        try {
            Library library;
            PdfFontAdapter font_adapter(font_directory);
            std::unique_ptr<PdfFileReader> file;
            FPDF_FILEACCESS file_access{};
#ifdef _WIN32
            // FPDF_FILEACCESS uses 32-bit unsigned long on Windows. Preserve
            // support for files above 4 GiB without copying them into RAM/disk.
            // Win32 forbids truncation while a mapped view exists:
            // https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-setendoffile
            std::unique_ptr<MappedFile> large_windows_file;
#endif
            FPDF_DOCUMENT document=nullptr;
            std::list<std::pair<PageIndex,std::shared_ptr<Page>>> pages;
            std::unique_ptr<ActiveRender> active;
            std::unique_ptr<TextExtraction> text_work;
            std::deque<TileKey> tiles;
            std::optional<ViewRequest> view;
            DocumentEpoch epoch=0;
            unsigned page_count=0,completed_tiles=0;
            auto release=[&]{active.reset();text_work.reset();{std::lock_guard lock(mutex);extracting.reset();}tiles.clear();view.reset();pages.clear();if(document)FPDF_CloseDocument(document);document=nullptr;file.reset();
#ifdef _WIN32
                large_windows_file.reset();
#endif
                page_count=0;};
            struct SessionGuard {decltype(release)& cleanup;~SessionGuard(){cleanup();}} session{release};
            auto get_page=[&](PageIndex index)->std::shared_ptr<Page>{
                if(!document||index>=page_count)throw std::runtime_error("ページ番号が範囲外です。");
                file->check();
                auto it=std::find_if(pages.begin(),pages.end(),[&](const auto& p){return p.first==index;});
                if(it!=pages.end()){auto p=it->second;pages.splice(pages.begin(),pages,it);return p;}
                auto p=std::make_shared<Page>();p->page=FPDF_LoadPage(document,static_cast<int>(index));
                file->check();
                if(!p->page)throw std::runtime_error("このページを読み込めません。別のページは引き続き開けます。");
                pages.emplace_front(index,p);
                // Render and extraction jobs own a reference for their duration.
                // Keep only the two most recent pages (and their image caches) idle.
                if(pages.size()>2)pages.pop_back();
                maximum(page_peak,pages.size());
                return p;
            };
            while(!stopping.load()) {
                std::unique_ptr<OpenRequest> open;
                std::optional<std::pair<DocumentEpoch,PageIndex>> page_request;
                std::optional<ViewRequest> viewport;
                {
                    std::lock_guard lock(mutex);
                    if(opening)open=std::move(opening);
                    else if(requested_page){page_request=requested_page;requested_page.reset();}
                    else if(requested_view){viewport=std::move(requested_view);requested_view.reset();}
                }
                PageIndex operation_page=view?view->page:0;
                std::uint64_t operation_query=0;
                bool text_operation=false;
                try {
                    if(open){
                        release();epoch=open->epoch;
                        if(open->path.empty()){PdfEvent e;e.kind=PdfEvent::Kind::Closed;e.document=epoch;emit(std::move(e));continue;}
                        auto snapshot=FileSnapshot::open(open->path);
                        if(!snapshot->initial_signature().size)throw std::runtime_error("空のファイルです。");
                        // A synchronous PDFium parse has no cancellation API.
                        // Finish that call, then discard stale work; deliberate
                        // callback read failures can violate parser invariants.
                        file=std::make_unique<PdfFileReader>(snapshot);
#ifdef _WIN32
                        if(snapshot->initial_signature().size>std::numeric_limits<unsigned long>::max()){
                            large_windows_file=std::make_unique<MappedFile>(open->path);
                            if(large_windows_file->initial_signature()!=snapshot->initial_signature())throw std::runtime_error("PDFが外部で変更されました。再度開いてください。");
                            file=std::make_unique<PdfFileReader>(large_windows_file->snapshot());
                            const auto bytes=large_windows_file->bytes();
                            document=FPDF_LoadMemDocument64(bytes.data(),bytes.size(),open->password.empty()?nullptr:open->password.c_str());
                        }else
#endif
                        {
                            if(snapshot->initial_signature().size>std::numeric_limits<unsigned long>::max())throw std::runtime_error("この環境のPDF読込APIで扱えるファイルサイズを超えています。");
                            file_access.m_FileLen=static_cast<unsigned long>(snapshot->initial_signature().size);
                            file_access.m_Param=file.get();
                            file_access.m_GetBlock=[](void* parameter,unsigned long offset,unsigned char* buffer,unsigned long size) noexcept ->int{
                                return static_cast<PdfFileReader*>(parameter)->read(offset,{reinterpret_cast<std::byte*>(buffer),size})?1:0;
                            };
                            document=FPDF_LoadCustomDocument(&file_access,open->password.empty()?nullptr:open->password.c_str());
                        }
                        clear_secret(open->password);
                        file->check();
                        if(!document){
                            const auto error=FPDF_GetLastError();release();
                            PdfEvent e;e.document=epoch;e.code="PdfOpen";e.detail=std::to_string(error);
                            e.kind=error==FPDF_ERR_PASSWORD?PdfEvent::Kind::PasswordRequired:PdfEvent::Kind::Failed;
                            e.message=error==FPDF_ERR_PASSWORD?"PDFのパスワードを入力してください。":"PDFを解析できません。破損または未対応の形式を確認してください。";
                            emit(std::move(e));continue;
                        }
                        const int count=FPDF_GetPageCount(document);
                        if(count<=0)throw std::runtime_error("PDFにページがありません。");
                        page_count=static_cast<unsigned>(count);
                        PdfEvent e;e.kind=PdfEvent::Kind::Opened;e.document=epoch;e.page_count=page_count;
                        e.file_signature=file->initial_signature();
                        e.file_snapshot=file->snapshot();
                        e.info=describe(get_page(0)->page);emit(std::move(e));continue;
                    }
                    if(page_request){
                        if(page_request->first==epoch){
                            active.reset();tiles.clear();view.reset();
                            operation_page=page_request->second;
                            PdfEvent e;e.kind=PdfEvent::Kind::PageInfo;e.document=epoch;e.page=operation_page;
                            e.info=describe(get_page(operation_page)->page);emit(std::move(e));
                        }else if(active)active->generation=request_generation.load();
                        continue;
                    }
                    if(viewport){
                        if(viewport->document!=epoch||!document){if(active)active->generation=request_generation.load();continue;}
                        operation_page=viewport->page;
                        if(viewport->width<=0||viewport->height<=0||viewport->width>max_page_pixels||
                           viewport->height>max_page_pixels||viewport->rotation<0||viewport->rotation>3)
                            throw std::runtime_error("描画寸法が不正です。");
                        view=std::move(viewport);operation_page=view->page;
                        if(active&&(!same_geometry(active->key,*view)||
                           std::find(view->tiles.begin(),view->tiles.end(),std::pair{active->key.x,active->key.y})==view->tiles.end()))active.reset();
                        if(active)active->generation=request_generation.load();
                        tiles.clear();
                        for(const auto& [x,y]:view->tiles){
                            if(tiles.size()>=64)break;
                            if(x<0||y<0||x>=view->width||y>=view->height||x%512||y%512)continue;
                            TileKey k{epoch,view->page,view->width,view->height,view->rotation,pdf_render_flags,x,y};
                            if((!active||active->key!=k)&&std::find(tiles.begin(),tiles.end(),k)==tiles.end())tiles.push_back(k);
                        }
                        PdfEvent e;e.kind=PdfEvent::Kind::ViewReady;e.document=epoch;e.page=view->page;
                        e.key={epoch,view->page,view->width,view->height,view->rotation,pdf_render_flags,0,0};
                        e.transform=transform(get_page(view->page)->page,view->width,view->height,view->rotation);emit(std::move(e));
                    }
                    if(active){
                        active->deadline=Clock::now()+std::chrono::milliseconds(10);
                        const auto slice_started=Clock::now();
                        active->status=FPDF_RenderPage_Continue(active->page->page,active.get());
                        file->check();
                        render_slice(slice_started,active->status);
                        if(active->status==FPDF_RENDER_TOBECONTINUED)continue;
                        if(active->status!=FPDF_RENDER_DONE)throw std::runtime_error("このタイルを描画できません。");
                        PdfEvent e;e.kind=PdfEvent::Kind::TileReady;e.document=epoch;e.page=active->key.page;
                        e.key=active->key;e.transform=active->mapping;e.pixels=active->pixels;
                        active.reset();emit(std::move(e));++completed_tiles;
                        continue;
                    }
                    if(text_work&&text_work->search&&text_work->search->generation!=desired_query.load())text_work.reset();
                    std::optional<std::pair<DocumentEpoch,PageIndex>> text_request;
                    std::optional<PdfSearchRequest> search_request;
                    std::optional<PageIndex> deferred;
                    if(tiles.empty()||completed_tiles>=4){
                        std::lock_guard lock(mutex);
                        // A visible-page request outranks background extraction
                        // or search. Requeue only the request, not a second raw
                        // glyph buffer, and continue after the prioritized page.
                        auto visible=std::find(extraction.begin(),extraction.end(),std::pair{epoch,displayed_page.load()});
                        if(text_work&&visible!=extraction.end()&&(text_work->search||text_work->index!=visible->second)){
                            if(text_work->search){if(!searching&&text_work->search->generation==desired_query.load())searching=std::move(text_work->search);}
                            else{
                                const auto previous=std::pair{epoch,text_work->index};
                                if(std::find(extraction.begin(),extraction.end(),previous)==extraction.end()){
                                    if(extraction.size()<64)extraction.push_back(previous);else deferred=text_work->index;
                                }
                                extracting.reset();
                            }
                            text_work.reset();
                        }
                        if(!text_work){
                            visible=std::find(extraction.begin(),extraction.end(),std::pair{epoch,displayed_page.load()});
                            if(visible!=extraction.end()){text_request=*visible;extraction.erase(visible);}
                            else if(!extraction.empty()){text_request=extraction.front();extraction.pop_front();}
                            else if(searching){search_request=std::move(searching);searching.reset();}
                        }
                    }
                    if(deferred){PdfEvent e;e.kind=PdfEvent::Kind::ExtractionDeferred;e.document=epoch;e.page=*deferred;emit(std::move(e));}
                    if(text_request){
                        text_operation=true;
                        if(text_request->first!=epoch)continue;
                        operation_page=text_request->second;
                        text_work=std::make_unique<TextExtraction>(get_page(operation_page),operation_page,layout_options);
                        {std::lock_guard lock(mutex);extracting=std::pair{epoch,operation_page};std::erase(extraction,*extracting);}
                    }
                    if(search_request){
                        text_operation=true;
                        if(search_request->document!=epoch)continue;
                        operation_page=search_request->page;operation_query=search_request->generation;
                        if(!file)throw std::runtime_error("PDFが閉じられました。");
                        file->check();
                        auto layout=search_request->cached_layout;
                        if(!layout||layout->page!=operation_page||layout->layout_version!=PageLayout::current_version)
                            text_work=std::make_unique<TextExtraction>(get_page(operation_page),operation_page,layout_options,std::move(search_request));
                        else {
                            PdfEvent e;e.kind=PdfEvent::Kind::SearchResult;e.document=epoch;e.page=operation_page;
                            e.query_generation=search_request->generation;e.layout=layout;
                            const auto cancelled=[&]{return stopping.load()||requested_document.load()!=epoch||desired_query.load()!=operation_query;};
                            e.hit=search_request->backward?find_text_backward(*layout,search_request->query,search_request->start,search_request->match_case,cancelled):find_text(*layout,search_request->query,search_request->start,search_request->match_case,cancelled);
                            emit(std::move(e));continue;
                        }
                    }
                    if(text_work&&(tiles.empty()||completed_tiles>=4)){
                        text_operation=true;
                        completed_tiles=0;operation_page=text_work->index;operation_query=text_work->search?text_work->search->generation:0;
                        if(!file)throw std::runtime_error("PDFが閉じられました。");
                        file->check();
                        ++text_slices;const bool complete=text_work->step(stopping,requested_document,epoch);if(!complete)++text_yields;
                        if(complete){
                            const auto query=operation_query;
                            const auto cancelled=[&]{return stopping.load()||requested_document.load()!=epoch||(query&&desired_query.load()!=query);};
                            auto extracted=text_work->finish(cancelled);file->check();PdfEvent e;e.document=epoch;e.page=operation_page;e.info=extracted.info;e.layout=std::move(extracted.layout);
                            if(text_work->search){const auto& s=*text_work->search;e.kind=PdfEvent::Kind::SearchResult;e.query_generation=s.generation;
                                e.hit=s.backward?find_text_backward(*e.layout,s.query,s.start,s.match_case,cancelled):find_text(*e.layout,s.query,s.start,s.match_case,cancelled);}
                            else e.kind=PdfEvent::Kind::Extracted;
                            if(!text_work->search){std::lock_guard lock(mutex);extracting.reset();}
                            text_work.reset();emit(std::move(e));
                        }
                        continue;
                    }
                    if(!tiles.empty()){
                        const auto key=tiles.front();tiles.pop_front();operation_page=key.page;
                        active=std::make_unique<ActiveRender>(this,request_generation.load());
                        active->key=key;active->page=get_page(key.page);
                        active->mapping=transform(active->page->page,key.full_width,key.full_height,key.rotation);
                        active->pixels=std::make_shared<RasterBuffer>();auto& r=*active->pixels;
                        r.width=std::min(512,key.full_width-key.x);r.height=std::min(512,key.full_height-key.y);r.stride=r.width*4;
                        r.bgra.resize(static_cast<std::size_t>(r.stride)*r.height,255);
                        maximum(tile_peak,r.bgra.size());
                        active->bitmap=FPDFBitmap_CreateEx(r.width,r.height,FPDFBitmap_BGRA,r.bgra.data(),r.stride);
                        if(!active->bitmap)throw std::runtime_error("描画用メモリを確保できません。");
                        FPDFBitmap_FillRect(active->bitmap,0,0,r.width,r.height,0xffffffff);
                        active->deadline=Clock::now()+std::chrono::milliseconds(10);active->started=true;
                        const auto slice_started=Clock::now();
                        active->status=FPDF_RenderPageBitmap_Start(active->bitmap,active->page->page,-key.x,-key.y,
                            key.full_width,key.full_height,key.rotation,key.flags,active.get());
                        file->check();
                        render_slice(slice_started,active->status);
                        if(active->status==FPDF_RENDER_DONE){
                            PdfEvent e;e.kind=PdfEvent::Kind::TileReady;e.document=epoch;e.page=key.page;e.key=key;
                            e.transform=active->mapping;e.pixels=active->pixels;active.reset();emit(std::move(e));++completed_tiles;
                        }else if(active->status!=FPDF_RENDER_TOBECONTINUED)throw std::runtime_error("このタイルを描画できません。");
                        continue;
                    }
                }catch(const LayoutCancelled&){
                    // Superseded searches/extractions are ordinary control
                    // flow, not a failed page in the current document.
                    text_work.reset();{std::lock_guard lock(mutex);extracting.reset();}
                    continue;
                }catch(const LayoutResourceLimit& error){
                    if(text_work&&text_work->owner->text){FPDFText_ClosePage(text_work->owner->text);text_work->owner->text=nullptr;}
                    text_work.reset();{std::lock_guard lock(mutex);extracting.reset();}
                    auto layout=std::make_shared<PageLayout>();layout->page=operation_page;layout->availability=PageLayout::Availability::ResourceLimited;
                    PdfEvent e;e.kind=operation_query?PdfEvent::Kind::SearchResult:PdfEvent::Kind::Extracted;e.document=epoch;e.page=operation_page;
                    e.layout=std::move(layout);e.query_generation=operation_query;e.code="PdfTextBudget";e.message=error.what();emit(std::move(e));continue;
                }catch(const std::exception& error){
                    if(open)release();
                    active.reset();
                    if(text_operation){text_work.reset();std::lock_guard lock(mutex);extracting.reset();}else tiles.clear();
                    PdfEvent e;e.kind=PdfEvent::Kind::Failed;e.document=epoch;e.page=operation_page;
                    e.code=operation_query?"PdfSearch":open?"PdfOpen":"PdfPage";e.query_generation=operation_query;e.message=error.what();emit(std::move(e));
                }
                std::unique_lock lock(mutex);
                cv.wait(lock,[&]{return stopping.load()||opening||requested_page||requested_view||searching||!extraction.empty()||text_work;});
            }
        }catch(...){
            stopping=true;
            try{PdfEvent e;e.kind=PdfEvent::Kind::Failed;e.document=requested_document.load();e.code="PdfService";e.message="PDF処理を続けられません。アプリを再起動してください。";emit(std::move(e));}catch(...){}
        }
        finished=true;
    }
};
PdfService::PdfService(Delivery callback,std::filesystem::path fonts,LayoutOptions limits):impl_(std::make_unique<Impl>(std::move(callback),std::move(fonts),limits)){}
PdfService::~PdfService()=default;
void PdfService::open(DocumentEpoch epoch,const std::filesystem::path& path,std::string_view password){
    std::lock_guard lock(impl_->mutex);impl_->opening=std::make_unique<Impl::OpenRequest>(epoch,path,password);
    impl_->requested_document=epoch;
    impl_->displayed_page=0;
    impl_->requested_view.reset();impl_->requested_page.reset();impl_->extraction.clear();impl_->searching.reset();
    ++impl_->request_generation;impl_->cv.notify_one();
}
void PdfService::close(DocumentEpoch epoch){open(epoch,{});}
void PdfService::page_info(DocumentEpoch epoch,PageIndex page){
    std::lock_guard lock(impl_->mutex);if(epoch!=impl_->requested_document.load()||impl_->stopping.load())return;impl_->requested_page=std::pair{epoch,page};
    impl_->displayed_page=page;
    ++impl_->request_generation;impl_->cv.notify_one();
}
void PdfService::viewport(ViewRequest request){
    if(request.tiles.size()>64)request.tiles.resize(64);
    std::lock_guard lock(impl_->mutex);if(request.document!=impl_->requested_document.load()||impl_->stopping.load())return;impl_->displayed_page=request.page;impl_->requested_view=std::move(request);
    ++impl_->request_generation;impl_->cv.notify_one();
}
bool PdfService::extract(DocumentEpoch epoch,PageIndex page){
    std::lock_guard lock(impl_->mutex);const auto key=std::pair{epoch,page};
    if(impl_->stopping.load()||epoch!=impl_->requested_document.load())return false;
    if(impl_->extracting==key)return true;
    if(std::find(impl_->extraction.begin(),impl_->extraction.end(),key)==impl_->extraction.end()){
        if(impl_->extraction.size()>=64)return false;
        impl_->extraction.push_back(key);
        Impl::maximum(impl_->queue_peak,impl_->extraction.size());
    }
    impl_->cv.notify_one();return true;
}
void PdfService::search(PdfSearchRequest request){
    std::lock_guard lock(impl_->mutex);if(request.document!=impl_->requested_document.load()||impl_->stopping.load())return;impl_->desired_query=request.generation;impl_->searching=std::move(request);impl_->cv.notify_one();
}
void PdfService::cancel_search(DocumentEpoch epoch,std::uint64_t generation){
    std::lock_guard lock(impl_->mutex);if(epoch!=impl_->requested_document.load()||impl_->stopping.load())return;
    impl_->desired_query=generation;impl_->searching.reset();impl_->cv.notify_one();
}
void PdfService::stop() noexcept{impl_->stop();}
bool PdfService::done() const noexcept{return impl_->finished.load();}
void PdfService::join(){if(impl_->worker.joinable())impl_->worker.join();}
PdfServiceStats PdfService::stats()const noexcept{
    const auto& p=*impl_;return {p.page_peak.load(),p.queue_peak.load(),p.tile_peak.load(),p.render_slices.load(),p.render_yields.load(),p.text_slices.load(),p.text_yields.load(),p.render_max_us.load()};
}
}
