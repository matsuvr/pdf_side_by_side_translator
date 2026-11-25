#include "pdf/restricted_pdf_service.hpp"
#include "platform/file_snapshot.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <windows.h>

using namespace std::chrono_literals;
using namespace opt;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
}
int main(int argc,char** argv){
    if(argc==5&&std::string_view(argv[1])==pdf_worker_switch)return run_pdf_worker(argv[2],argv[3],argv[4]);
    if(argc!=2)return 2;
    try{
        // Normal-integrity command-line invocation cannot enter parser mode.
        require(run_pdf_worker("1","2","3")!=0,"Worker accepted a direct uncontained invocation");
        const auto fixture=std::filesystem::u8path(argv[1]);
        std::mutex mutex;std::condition_variable cv;std::deque<std::shared_ptr<const PdfEvent>> events;
        const auto main=std::this_thread::get_id();std::atomic_bool wrong_thread{false};
        RestrictedPdfService service([&](auto event){
            if(std::this_thread::get_id()==main)wrong_thread=true;
            std::lock_guard lock(mutex);events.push_back(std::move(event));cv.notify_one();
        });
        auto wait=[&](PdfEvent::Kind kind,DocumentEpoch epoch){
            const auto until=std::chrono::steady_clock::now()+30s;
            std::unique_lock lock(mutex);
            for(;;){
                while(!events.empty()){
                    auto e=std::move(events.front());events.pop_front();if(e->document!=epoch)continue;
                    if(e->kind==kind)return e;
                    if(e->kind==PdfEvent::Kind::Failed)throw std::runtime_error(e->message);
                }
                if(cv.wait_until(lock,until)==std::cv_status::timeout)throw std::runtime_error("Timed out waiting for isolated PDF worker");
            }
        };
        service.open(1,fixture);
        auto opened=wait(PdfEvent::Kind::Opened,1);
        require(opened->page_count==3&&opened->file_snapshot&&opened->file_snapshot->matches_initial_signature(),"Parent snapshot and PDF metadata");
        const auto pid=service.worker_process_id();require(pid&&pid!=GetCurrentProcessId(),"PDF parser has a different process identity");
        using Handle=std::unique_ptr<void,decltype(&CloseHandle)>;
        Handle worker(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE,FALSE,pid),CloseHandle);
        require(bool(worker),"Open own parser process for diagnostic test");
        BOOL in_job=FALSE;require(IsProcessInJob(worker.get(),nullptr,&in_job)&&in_job,"Parser belongs to process job");
        HANDLE raw=nullptr;require(OpenProcessToken(worker.get(),TOKEN_QUERY,&raw),"Read own worker token");
        Handle token(raw,CloseHandle);DWORD bytes=0;GetTokenInformation(token.get(),TokenIntegrityLevel,nullptr,0,&bytes);
        require(bytes>=sizeof(TOKEN_MANDATORY_LABEL),"Read worker integrity size");
        std::vector<unsigned char> label(bytes);require(GetTokenInformation(token.get(),TokenIntegrityLevel,label.data(),bytes,&bytes),"Read worker integrity");
        auto sid=reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
        require(IsValidSid(sid)&&*GetSidSubAuthorityCount(sid)&&*GetSidSubAuthority(sid,*GetSidSubAuthorityCount(sid)-1)==SECURITY_MANDATORY_LOW_RID,"Worker runs at low integrity");
        service.viewport({1,0,1224,1584,0,{{0,0}}});
        wait(PdfEvent::Kind::ViewReady,1);
        auto tile=wait(PdfEvent::Kind::TileReady,1);
        require(tile->pixels&&tile->pixels->bgra.size()==512*512*4,"Isolated render tile");
        require(service.extract(1,0),"Isolated text request");
        auto layout=wait(PdfEvent::Kind::Extracted,1)->layout;
        require(layout&&layout->glyphs.size()==222,"Isolated PDF glyph extraction");
        service.search({1,0,1,"NETWORK",0,false,false,layout});
        require(wait(PdfEvent::Kind::SearchResult,1)->hit.has_value(),"Same cached-layout search behavior");
        service.search({1,0,2,"NETWORK",0,false,false,{}});
        require(wait(PdfEvent::Kind::SearchResult,1)->hit.has_value(),"Uncached worker search");
        require(service.render_page_image(1,0,0,3),"OCR image request");
        auto image=wait(PdfEvent::Kind::PageImage,1);
        require(image->pixels&&image->pixels->stride>=image->pixels->width,"Isolated grayscale raster for OCR");
        // A crash/forced termination of our child is confined to this
        // session; the UI-side service can still open another document.
        require(TerminateProcess(worker.get(),1),"Terminate test-owned parser process");
        require(wait(PdfEvent::Kind::Failed,1)->code=="PdfService","Parser failure is reported");
        worker.reset();token.reset();
        service.open(9,fixture);wait(PdfEvent::Kind::Opened,9);
        for(int i=0;i<100;++i)service.viewport({9,1,1200+i,1600+i,0,{{0,0}}});
        service.open(2,fixture.parent_path()/"P11-password.pdf");
        wait(PdfEvent::Kind::PasswordRequired,2);
        service.open(2,fixture.parent_path()/"P11-password.pdf","incorrect-fixture-password");
        wait(PdfEvent::Kind::PasswordRequired,2);
        service.open(2,fixture.parent_path()/"P11-password.pdf","reader-fixture");
        wait(PdfEvent::Kind::Opened,2);
        require(service.extract(2,0),"Encrypted PDF request");
        require(wait(PdfEvent::Kind::Extracted,2)->layout->reading_text_utf8.find("there are 11 samples")!=std::string::npos,"Password retry");
        service.close(3);wait(PdfEvent::Kind::Closed,3);
        service.stop();service.join();
        require(service.done()&&!wrong_thread.load(),"Bounded asynchronous worker shutdown");
        std::cout<<"Windows restricted PDF process open/render/extract/search/OCR/password and cancellation passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
