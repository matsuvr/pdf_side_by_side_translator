#include "pdf/pdf_service.hpp"
#include "platform/mapped_file.hpp"
#include "pdf/layout.hpp"
#include "fpdfview.h"
#include "fpdf_text.h"
#include "fpdf_progressive.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace opt {
namespace {
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
    const int normalized_height=std::clamp(static_cast<int>(std::min(1000000.0,1024.0*h/w)),1,1000000);
    return {w,h,transform(page,1024,normalized_height,0).pdf_to_page_px,1024.0,static_cast<double>(normalized_height)};
}
struct ExtractedPage {PdfPageInfo info;std::shared_ptr<const PageLayout> layout;};
ExtractedPage extract_page(Page& page,PageIndex index,const std::atomic_bool& stopping,
                           const std::atomic<DocumentEpoch>& requested_document,DocumentEpoch epoch){
    if(!page.text)page.text=FPDFText_LoadPage(page.page);
    if(!page.text)throw std::runtime_error("このページの文字を抽出できません。");
    std::vector<GlyphRecord> glyphs;
    const int count=FPDFText_CountChars(page.text);
    if(count<0)throw std::runtime_error("文字数を取得できません。");
    glyphs.reserve(static_cast<std::size_t>(count));
    for(int i=0;i<count&&!stopping.load();++i){
        if(i%256==0&&requested_document.load()!=epoch)throw std::runtime_error("ページの抽出を中断しました。");
        GlyphRecord g;g.index=static_cast<PdfCharIndex>(i);
        g.unicode=static_cast<char32_t>(FPDFText_GetUnicode(page.text,i));
        g.valid_bounds=FPDFText_GetCharBox(page.text,i,&g.bounds.left,&g.bounds.right,&g.bounds.bottom,&g.bounds.top);
        FPDFText_GetCharOrigin(page.text,i,&g.origin.x,&g.origin.y);
        g.font_size=FPDFText_GetFontSize(page.text,i);g.angle_radians=FPDFText_GetCharAngle(page.text,i);
        g.generated=FPDFText_IsGenerated(page.text,i)==1;g.unicode_map_error=FPDFText_HasUnicodeMapError(page.text,i)==1;
        std::array<char,128> buffer{};int flags=0;
        const auto length=FPDFText_GetFontInfo(page.text,i,buffer.data(),buffer.size(),&flags);
        std::string large;
        const char* name=buffer.data();
        if(length>buffer.size()){
            large.resize(length);name=large.data();
            if(FPDFText_GetFontInfo(page.text,i,large.data(),length,&flags)!=length)name=nullptr;
        }
        if(name&&length){
            std::uint32_t hash=2166136261U;
            for(unsigned long j=0;j<length;++j){hash^=static_cast<unsigned char>(name[j]);hash*=16777619U;}
            g.font_id=hash^static_cast<std::uint32_t>(flags);
        }
        glyphs.push_back(g);
    }
    if(stopping.load()||requested_document.load()!=epoch)throw std::runtime_error("ページの抽出を中断しました。");
    const auto info=describe(page.page);
    return {info,std::make_shared<const PageLayout>(LayoutEngine{}.build(index,std::move(glyphs),
        info.pdf_to_layout,info.layout_width,info.layout_height))};
}
bool same_geometry(const TileKey& a,const ViewRequest& b) {
    return a.document==b.document&&a.page==b.page&&a.full_width==b.width&&
        a.full_height==b.height&&a.rotation==b.rotation;
}
struct Library {Library(){FPDF_LIBRARY_CONFIG c{};c.version=2;FPDF_InitLibraryWithConfig(&c);}~Library(){FPDF_DestroyLibrary();}};
}

struct PdfService::Impl {
    struct OpenRequest {DocumentEpoch epoch;std::filesystem::path path;std::string password;};
    Delivery delivery;
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<OpenRequest> opening;
    std::optional<std::pair<DocumentEpoch,PageIndex>> requested_page;
    std::optional<ViewRequest> requested_view;
    std::optional<PdfSearchRequest> searching;
    std::deque<std::pair<DocumentEpoch,PageIndex>> extraction;
    std::atomic_bool stopping{false},finished{false};
    std::atomic<std::uint64_t> request_generation{0};
    std::atomic<DocumentEpoch> requested_document{0};
    std::thread worker;
    explicit Impl(Delivery callback):delivery(std::move(callback)),worker([this]{run();}){}
    ~Impl(){stopping=true;cv.notify_one();if(worker.joinable())worker.join();}
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
            std::unique_ptr<MappedFile> file;
            FPDF_DOCUMENT document=nullptr;
            std::list<std::pair<PageIndex,std::shared_ptr<Page>>> pages;
            std::unique_ptr<ActiveRender> active;
            std::deque<TileKey> tiles;
            std::optional<ViewRequest> view;
            DocumentEpoch epoch=0;
            unsigned page_count=0,completed_tiles=0;
            auto release=[&]{active.reset();tiles.clear();view.reset();pages.clear();if(document)FPDF_CloseDocument(document);document=nullptr;file.reset();page_count=0;};
            struct SessionGuard {decltype(release)& release;~SessionGuard(){release();}} session{release};
            auto get_page=[&](PageIndex index)->std::shared_ptr<Page>{
                if(!document||index>=page_count)throw std::runtime_error("ページ番号が範囲外です。");
                if(file->signature()!=file->initial_signature())throw std::runtime_error("PDFが外部で変更されました。再度開いてください。");
                auto it=std::find_if(pages.begin(),pages.end(),[&](const auto& p){return p.first==index;});
                if(it!=pages.end()){auto p=it->second;pages.splice(pages.begin(),pages,it);return p;}
                auto p=std::make_shared<Page>();p->page=FPDF_LoadPage(document,static_cast<int>(index));
                if(!p->page)throw std::runtime_error("このページを読み込めません。別のページは引き続き開けます。");
                pages.emplace_front(index,p);
                while(pages.size()>8){
                    auto victim=std::find_if(pages.rbegin(),pages.rend(),[](const auto& v){return v.second.use_count()==1;});
                    if(victim==pages.rend())break;
                    pages.erase(std::next(victim).base());
                }
                return p;
            };
            while(!stopping.load()) {
                std::optional<OpenRequest> open;
                std::optional<std::pair<DocumentEpoch,PageIndex>> page_request;
                std::optional<ViewRequest> viewport;
                {
                    std::lock_guard lock(mutex);
                    if(opening){open=std::move(opening);opening.reset();}
                    else if(requested_page){page_request=requested_page;requested_page.reset();}
                    else if(requested_view){viewport=std::move(requested_view);requested_view.reset();}
                }
                PageIndex operation_page=view?view->page:0;
                std::uint64_t operation_query=0;
                try {
                    if(open){
                        release();epoch=open->epoch;
                        if(open->path.empty()){PdfEvent e;e.kind=PdfEvent::Kind::Closed;e.document=epoch;emit(std::move(e));continue;}
                        file=std::make_unique<MappedFile>(open->path);
                        auto bytes=file->bytes();
                        document=FPDF_LoadMemDocument64(bytes.data(),bytes.size(),open->password.empty()?nullptr:open->password.c_str());
                        std::fill(open->password.begin(),open->password.end(),'\0');
                        if(!document){
                            const auto error=FPDF_GetLastError();file.reset();
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
                        try{e.file_snapshot=file->snapshot();}catch(const std::exception&){e.detail="Fingerprint file object unavailable";}
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
                        if(viewport->width<=0||viewport->height<=0||viewport->rotation<0||viewport->rotation>3)
                            throw std::runtime_error("描画寸法が不正です。");
                        view=std::move(viewport);operation_page=view->page;
                        if(active&&(!same_geometry(active->key,*view)||
                           std::find(view->tiles.begin(),view->tiles.end(),std::pair{active->key.x,active->key.y})==view->tiles.end()))active.reset();
                        if(active)active->generation=request_generation.load();
                        tiles.clear();
                        for(const auto& [x,y]:view->tiles){
                            if(tiles.size()>=64)break;
                            if(x<0||y<0||x>=view->width||y>=view->height||x%512||y%512)continue;
                            TileKey k{epoch,view->page,view->width,view->height,view->rotation,FPDF_ANNOT,x,y};
                            if((!active||active->key!=k)&&std::find(tiles.begin(),tiles.end(),k)==tiles.end())tiles.push_back(k);
                        }
                        PdfEvent e;e.kind=PdfEvent::Kind::ViewReady;e.document=epoch;e.page=view->page;
                        e.key={epoch,view->page,view->width,view->height,view->rotation,FPDF_ANNOT,0,0};
                        e.transform=transform(get_page(view->page)->page,view->width,view->height,view->rotation);emit(std::move(e));
                    }
                    if(active){
                        active->deadline=Clock::now()+std::chrono::milliseconds(10);
                        active->status=FPDF_RenderPage_Continue(active->page->page,active.get());
                        if(active->status==FPDF_RENDER_TOBECONTINUED)continue;
                        if(active->status!=FPDF_RENDER_DONE)throw std::runtime_error("このタイルを描画できません。");
                        PdfEvent e;e.kind=PdfEvent::Kind::TileReady;e.document=epoch;e.page=active->key.page;
                        e.key=active->key;e.transform=active->mapping;e.pixels=active->pixels;
                        active.reset();emit(std::move(e));++completed_tiles;
                        continue;
                    }
                    std::optional<std::pair<DocumentEpoch,PageIndex>> text_request;
                    std::optional<PdfSearchRequest> search_request;
                    if(tiles.empty()||completed_tiles>=4){
                        std::lock_guard lock(mutex);
                        if(!extraction.empty()){text_request=extraction.front();extraction.pop_front();completed_tiles=0;}
                        else if(searching){search_request=std::move(searching);searching.reset();completed_tiles=0;}
                    }
                    if(text_request){
                        if(text_request->first!=epoch)continue;
                        operation_page=text_request->second;
                        auto extracted=extract_page(*get_page(operation_page),operation_page,stopping,requested_document,epoch);
                        PdfEvent e;e.kind=PdfEvent::Kind::Extracted;e.document=epoch;e.page=operation_page;
                        e.info=extracted.info;e.layout=std::move(extracted.layout);
                        emit(std::move(e));continue;
                    }
                    if(search_request){
                        if(search_request->document!=epoch)continue;
                        operation_page=search_request->page;operation_query=search_request->generation;
                        if(!file||file->signature()!=file->initial_signature())throw std::runtime_error("PDFが外部で変更されました。再度開いてください。");
                        auto layout=search_request->cached_layout;
                        if(!layout||layout->page!=operation_page||layout->layout_version!=1)
                            layout=extract_page(*get_page(operation_page),operation_page,stopping,requested_document,epoch).layout;
                        PdfEvent e;e.kind=PdfEvent::Kind::SearchResult;e.document=epoch;e.page=operation_page;
                        e.query_generation=search_request->generation;e.layout=layout;
                        e.hit=search_request->backward?find_text_backward(*layout,search_request->query,search_request->start,search_request->match_case):
                            find_text(*layout,search_request->query,search_request->start,search_request->match_case);
                        emit(std::move(e));continue;
                    }
                    if(!tiles.empty()){
                        const auto key=tiles.front();tiles.pop_front();operation_page=key.page;
                        active=std::make_unique<ActiveRender>(this,request_generation.load());
                        active->key=key;active->page=get_page(key.page);
                        active->mapping=transform(active->page->page,key.full_width,key.full_height,key.rotation);
                        active->pixels=std::make_shared<RasterBuffer>();auto& r=*active->pixels;
                        r.width=std::min(512,key.full_width-key.x);r.height=std::min(512,key.full_height-key.y);r.stride=r.width*4;
                        r.bgra.resize(static_cast<std::size_t>(r.stride)*r.height,255);
                        active->bitmap=FPDFBitmap_CreateEx(r.width,r.height,FPDFBitmap_BGRA,r.bgra.data(),r.stride);
                        if(!active->bitmap)throw std::runtime_error("描画用メモリを確保できません。");
                        FPDFBitmap_FillRect(active->bitmap,0,0,r.width,r.height,0xffffffff);
                        active->deadline=Clock::now()+std::chrono::milliseconds(10);active->started=true;
                        active->status=FPDF_RenderPageBitmap_Start(active->bitmap,active->page->page,-key.x,-key.y,
                            key.full_width,key.full_height,key.rotation,key.flags,active.get());
                        if(active->status==FPDF_RENDER_DONE){
                            PdfEvent e;e.kind=PdfEvent::Kind::TileReady;e.document=epoch;e.page=key.page;e.key=key;
                            e.transform=active->mapping;e.pixels=active->pixels;active.reset();emit(std::move(e));++completed_tiles;
                        }else if(active->status!=FPDF_RENDER_TOBECONTINUED)throw std::runtime_error("このタイルを描画できません。");
                        continue;
                    }
                }catch(const std::exception& error){
                    active.reset();tiles.clear();
                    PdfEvent e;e.kind=PdfEvent::Kind::Failed;e.document=epoch;e.page=operation_page;
                    e.code=operation_query?"PdfSearch":open?"PdfOpen":"PdfPage";e.query_generation=operation_query;e.message=error.what();emit(std::move(e));
                }
                std::unique_lock lock(mutex);
                cv.wait(lock,[&]{return stopping.load()||opening||requested_page||requested_view||searching||!extraction.empty();});
            }
        }catch(...){
            try{PdfEvent e;e.code="PdfService";e.message="PDF処理を続けられません。アプリを再起動してください。";emit(std::move(e));}catch(...){}
        }
        finished=true;
    }
};
PdfService::PdfService(Delivery callback):impl_(std::make_unique<Impl>(std::move(callback))){}
PdfService::~PdfService()=default;
void PdfService::open(DocumentEpoch epoch,const std::filesystem::path& path,std::string password){
    std::lock_guard lock(impl_->mutex);impl_->opening=Impl::OpenRequest{epoch,path,std::move(password)};
    impl_->requested_document=epoch;
    impl_->requested_view.reset();impl_->requested_page.reset();impl_->extraction.clear();impl_->searching.reset();
    ++impl_->request_generation;impl_->cv.notify_one();
}
void PdfService::close(DocumentEpoch epoch){open(epoch,{});}
void PdfService::page_info(DocumentEpoch epoch,PageIndex page){
    std::lock_guard lock(impl_->mutex);impl_->requested_page=std::pair{epoch,page};
    ++impl_->request_generation;impl_->cv.notify_one();
}
void PdfService::viewport(ViewRequest request){
    if(request.tiles.size()>64)request.tiles.resize(64);
    std::lock_guard lock(impl_->mutex);impl_->requested_view=std::move(request);
    ++impl_->request_generation;impl_->cv.notify_one();
}
bool PdfService::extract(DocumentEpoch epoch,PageIndex page){
    std::lock_guard lock(impl_->mutex);const auto key=std::pair{epoch,page};
    if(std::find(impl_->extraction.begin(),impl_->extraction.end(),key)==impl_->extraction.end()){
        if(impl_->extraction.size()>=64)return false;
        impl_->extraction.push_back(key);
    }
    impl_->cv.notify_one();return true;
}
void PdfService::search(PdfSearchRequest request){
    std::lock_guard lock(impl_->mutex);impl_->searching=std::move(request);impl_->cv.notify_one();
}
void PdfService::stop() noexcept{impl_->stopping=true;impl_->cv.notify_one();}
bool PdfService::done() const noexcept{return impl_->finished.load();}
void PdfService::join(){if(impl_->worker.joinable())impl_->worker.join();}
}
