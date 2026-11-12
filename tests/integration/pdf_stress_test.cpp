#include "pdf/pdf_service.hpp"
#include "platform/access_path.hpp"
#include "platform/digest.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace opt;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::filesystem::path path(const char* s){return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s)));}
class Inbox {
    std::mutex mutex;std::condition_variable cv;std::deque<std::shared_ptr<const PdfEvent>> events;
public:
    void push(std::shared_ptr<const PdfEvent> e){std::lock_guard lock(mutex);events.push_back(std::move(e));cv.notify_one();}
    template<class Predicate> auto wait(Predicate match,bool accept_failure=false){
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);std::unique_lock lock(mutex);
        for(;;){
            if(!cv.wait_until(lock,deadline,[&]{return !events.empty();}))throw std::runtime_error("PDF stress event timeout");
            auto e=events.front();events.pop_front();
            if(e->kind==PdfEvent::Kind::Failed&&!accept_failure)throw std::runtime_error(e->message);
            if(e->pixels)require(e->pixels->bgra.size()<=1024*1024,"tile exceeds 512-pixel bound");
            if(match(*e))return e;
        }
    }
    auto wait(DocumentEpoch doc,PdfEvent::Kind kind,PageIndex page=0){return wait([&](const auto& e){return e.document==doc&&e.kind==kind&&e.page==page;});}
};
int main(int argc,char** argv){
    if(argc!=2)return 2;const auto fixtures=path(argv[1]);
    const auto temporary=std::filesystem::temp_directory_path()/std::filesystem::path("opt-native-path-"+new_uuid());
    try{
        Inbox inbox;PdfService service([&](auto e){inbox.push(std::move(e));});
        auto started=std::chrono::steady_clock::now();
        service.open(1,fixtures/"P09-large.pdf");auto opened=inbox.wait(1,PdfEvent::Kind::Opened);
        require(opened->page_count==300&&opened->info.width_points==30000,"large physical pages opened");
        service.viewport({1,0,60000,60000,0,{{0,0},{512,0},{0,512},{512,512}}});inbox.wait(1,PdfEvent::Kind::TileReady);
        const auto first_tile_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        service.extract(1,0);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(service.stats().text_slices==0&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(service.stats().text_slices>0,"large extraction started on worker");
        // Preserve the partial extractor while rendering a different page;
        // prioritize its short extraction, then resume the earlier source.
        service.page_info(1,299);service.viewport({1,299,60000,60000,0,{{0,0}}});service.extract(1,299);
        auto last=inbox.wait(1,PdfEvent::Kind::Extracted,299);require(last->layout->reading_text_utf8.find("Large page 0300")!=std::string::npos,"latest visible page extracted first");
        service.extract(1,0);auto full=inbox.wait(1,PdfEvent::Kind::Extracted,0);
        require(full->layout->reading_text_utf8.find("Incremental row 1000")!=std::string::npos,"interrupted large extraction resumes without source loss");
        full.reset();last.reset();
        for(PageIndex page=1;page<=12;++page){service.page_info(1,page);inbox.wait(1,PdfEvent::Kind::PageInfo,page);service.viewport({1,page,60000,60000,0,{{0,0}}});inbox.wait(1,PdfEvent::Kind::TileReady,page);}
        auto before=service.stats();require(before.page_cache_peak==2,"page/text handle LRU bounded to two");require(before.tile_bytes_peak<=1024*1024,"large pages never allocate whole-page bitmap");require(before.text_yields>0,"dense source extraction uses cooperative slices");
        service.open(2,fixtures/"P10-complex.pdf");inbox.wait(2,PdfEvent::Kind::Opened);
        service.viewport({2,0,4096,5301,0,{{0,0},{512,0},{0,512},{512,512}}});inbox.wait(2,PdfEvent::Kind::ViewReady);
        service.viewport({2,0,800,1035,1,{{0,0}}});
        auto latest=inbox.wait([&](const auto& e){return e.document==2&&e.kind==PdfEvent::Kind::TileReady&&e.key.full_width==800&&e.key.rotation==1;});
        require(latest->pixels->width<=512&&latest->key.rotation==1,"complex render switches to latest geometry");
        service.open(3,fixtures/"P11-password.pdf");inbox.wait(3,PdfEvent::Kind::PasswordRequired);
        service.open(3,fixtures/"P11-password.pdf","incorrect-fixture-password");inbox.wait(3,PdfEvent::Kind::PasswordRequired);
        service.close(4);inbox.wait(4,PdfEvent::Kind::Closed);
        service.open(5,fixtures/"P11-password.pdf","reader-fixture");inbox.wait(5,PdfEvent::Kind::Opened);service.extract(5,0);
        require(inbox.wait(5,PdfEvent::Kind::Extracted)->layout->reading_text_utf8.find("there are 11 samples")!=std::string::npos,"correct password succeeds after wrong input and cancellation");
        auto folder=temporary;
        for(unsigned i=0;i<10;++i)folder/=std::filesystem::path(u8"日本語 と 空白のフォルダー")/std::filesystem::path(std::to_string(i)+"-long-native-path");
        std::filesystem::create_directories(access_path(folder));const auto target=folder/std::filesystem::path(u8"読書 と 翻訳.pdf");
        std::filesystem::copy_file(fixtures/"P01-simple.pdf",access_path(target));
        require(target.u8string().size()>260,"native path fixture is long as well as non-ASCII");
        service.open(6,target);inbox.wait(6,PdfEvent::Kind::Opened);service.extract(6,0);
        require(inbox.wait(6,PdfEvent::Kind::Extracted)->layout->reading_text_utf8.find("3 sensors")!=std::string::npos,"long Japanese/space path reaches PDFium end to end");
        const auto changed=temporary/"externally-changed.pdf";
        std::filesystem::copy_file(fixtures/"P01-simple.pdf",changed);
        service.open(7,changed);inbox.wait(7,PdfEvent::Kind::Opened);
        service.viewport({7,0,max_page_pixels+1,612,0,{{0,0}}});
        require(inbox.wait([](const auto& e){return e.document==7&&e.kind==PdfEvent::Kind::Failed;},true)->code=="PdfPage","Oversize viewport reached renderer");
#ifdef _WIN32
        std::error_code blocked;std::filesystem::resize_file(changed,0,blocked);
        require(bool(blocked),"PDF worker allowed external truncation");
        service.viewport({7,0,612,792,0,{{0,0}}});inbox.wait(7,PdfEvent::Kind::TileReady);
#else
        std::filesystem::resize_file(changed,0);
        service.viewport({7,0,612,792,0,{{0,0}}});
        auto failure=inbox.wait([](const auto& e){return e.document==7&&e.kind==PdfEvent::Kind::Failed;},true);
        require(failure->code=="PdfPage","External truncation should fail only the changed document operation");
#endif
        service.open(8,fixtures/"P01-simple.pdf");inbox.wait(8,PdfEvent::Kind::Opened);service.extract(8,0);
        require(inbox.wait(8,PdfEvent::Kind::Extracted)->layout->reading_text_utf8.find("3 sensors")!=std::string::npos,"PDF worker did not recover after input truncation");
        service.stop();service.join();
        const auto stats=service.stats();std::cout<<"first_large_tile_ms="<<first_tile_ms<<" page_cache_peak="<<stats.page_cache_peak<<" tile_bytes_peak="<<stats.tile_bytes_peak
            <<" render_slices="<<stats.render_slices<<" render_yields="<<stats.render_yields<<" max_render_slice_us="<<stats.max_render_slice_us<<" text_slices="<<stats.text_slices<<" text_yields="<<stats.text_yields<<'\n';
        std::filesystem::remove_all(access_path(temporary));
    }catch(const std::exception& e){std::cerr<<e.what()<<" (temporary fixture at "<<temporary<<")\n";return 1;}
}
