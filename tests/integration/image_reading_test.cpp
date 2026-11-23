#include "ocr/image_reading.hpp"
#include "image_reading_assets.hpp"
#include "pdf/pdf_service.hpp"
#include "blread.h"
#include "ocrblock.h"
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
constexpr wchar_t delay_event_var[]=L"OPT_TEST_OSD_STARTED";
bool test_model_reader(const char* name,std::vector<char>* data){
    wchar_t event_name[128]{};
    if(std::string_view(name).ends_with("osd.traineddata") &&
       GetEnvironmentVariableW(delay_event_var,event_name,std::size(event_name))){
        HANDLE started=OpenEventW(EVENT_MODIFY_STATE,FALSE,event_name);
        if(!started)return false;
        const auto mutex_name=std::wstring(event_name)+L"-lifetime";
        std::unique_ptr<void,decltype(&CloseHandle)> lifetime(OpenMutexW(SYNCHRONIZE,FALSE,mutex_name.c_str()),CloseHandle);
        if(!lifetime||WaitForSingleObject(lifetime.get(),1000)!=WAIT_OBJECT_0){CloseHandle(started);return false;}
        SetEvent(started);
        CloseHandle(started);
        Sleep(30000); // Simulate a model reader blocked inside Tesseract.
    }
    return opt::read_bundled_image_model(name,data);
}
void check_interruptible_detection(opt::GrayPage page){
    using namespace std::chrono_literals;
    const std::wstring name=L"Local\\opt-ocr-test-"+std::to_wstring(GetCurrentProcessId());
    std::unique_ptr<void,decltype(&CloseHandle)> started(CreateEventW(nullptr,TRUE,FALSE,name.c_str()),CloseHandle);
    std::unique_ptr<void,decltype(&CloseHandle)> lifetime(CreateMutexW(nullptr,FALSE,(name+L"-lifetime").c_str()),CloseHandle);
    if(!started||!lifetime||!SetEnvironmentVariableW(delay_event_var,name.c_str()))throw std::runtime_error("OSD test setup failed");
    auto check_worker_exit=[&]{
        if(WaitForSingleObject(lifetime.get(),2000)!=WAIT_ABANDONED)
            throw std::runtime_error("Stopped OCR worker remained alive");
        ReleaseMutex(lifetime.get());
    };
    std::stop_source stop;
    std::jthread cancel([&]{
        WaitForSingleObject(started.get(),5000);
        stop.request_stop();
    });
    const auto first=std::chrono::steady_clock::now();
    auto canceled=opt::read_page_image(page,"",stop.get_token());
    cancel.join();
    if(!canceled.text.empty()||WaitForSingleObject(started.get(),0)!=WAIT_OBJECT_0||
       std::chrono::steady_clock::now()-first>6s)throw std::runtime_error("Could not cancel active script detection");
    check_worker_exit();
    ResetEvent(started.get());
    const auto deadline=std::chrono::steady_clock::now()+4s;
    bool timed_out=false;
    try{opt::read_page_image(page,"",{},deadline);}
    catch(const opt::ImageReadingTimeout&){timed_out=true;}
    SetEnvironmentVariableW(delay_event_var,nullptr);
    if(!timed_out||WaitForSingleObject(started.get(),0)!=WAIT_OBJECT_0||
       std::chrono::steady_clock::now()>deadline+2s)throw std::runtime_error("OSD exceeded its deadline");
    check_worker_exit();
}
}

int main(int argc,char** argv){
    if(argc==3&&argv[1]==std::string_view(opt::image_worker_switch))
        return opt::run_image_worker(argv[2],test_model_reader);
    if(argc!=2)return 2;
    using namespace opt;
    using namespace std::chrono_literals;
    try{
        auto zone=std::filesystem::u8path(argv[1]).parent_path()/"ocr-zone";
        if(!std::filesystem::is_regular_file(zone.string()+".uzn"))throw std::runtime_error("Missing zone fixture");
        auto zone_name=zone.string();
        tesseract::BLOCK_LIST blocks;
        if(tesseract::read_unlv_file(zone_name,160,160,&blocks)||!blocks.empty()||zone_name!=zone.string())
            throw std::runtime_error("Embedded Tesseract consulted the external zone reader");
        std::mutex lock;std::condition_variable cv;
        std::deque<std::shared_ptr<const PdfEvent>> events;
        PdfService pdf([&](auto e){std::lock_guard guard(lock);events.push_back(std::move(e));cv.notify_one();});
        auto wait=[&](PdfEvent::Kind kind){
            const auto deadline=std::chrono::steady_clock::now()+12s;
            std::unique_lock guard(lock);
            while(true){
                while(!events.empty()){
                    auto e=events.front();events.pop_front();
                    if(e->kind==PdfEvent::Kind::Failed)throw std::runtime_error(e->message);
                    if(e->kind==kind)return e;
                }
                if(cv.wait_until(guard,deadline)==std::cv_status::timeout)
                    throw std::runtime_error("Timed out waiting for the PDF owner");
            }
        };
        pdf.open(1,std::filesystem::u8path(argv[1]));
        wait(PdfEvent::Kind::Opened);
        if(pdf.render_page_image(2,0,0,1)||pdf.render_page_image(1,1,0,1)||
           pdf.render_page_image(1,0,4,1)||!pdf.render_page_image(1,0,0,42))
            throw std::runtime_error("Page image request identity was ignored");
        auto e=wait(PdfEvent::Kind::PageImage);
        if(!e->pixels||e->document!=1||e->page!=0||e->image_generation!=42)
            throw std::runtime_error("Page image render failed: "+e->message);
        auto& p=*e->pixels;
        if(e->image_dpi!=220||p.stride<p.width||p.width*p.height>image_reading_max_area||
           p.bgra.size()!=static_cast<std::size_t>(p.stride)*p.height)
            throw std::runtime_error("Unexpected page raster layout");
        const auto read=[&](std::string_view language,std::stop_token stop={}){
            return read_page_image({p.bgra,p.width,p.height,p.stride,e->image_dpi},language,stop);
        };
        auto short_page=std::span(p.bgra).first(p.bgra.size()-1);
        try{
            read_page_image({short_page,p.width,p.height,p.stride,e->image_dpi},"en",{});
            throw std::runtime_error("Truncated raster was accepted");
        }catch(const std::invalid_argument&){}
        check_interruptible_detection({p.bgra,p.width,p.height,p.stride,e->image_dpi});
        std::stop_source canceled;canceled.request_stop();
        if(!read("en",canceled.get_token()).text.empty())
            throw std::runtime_error("Canceled image request produced text");
        if(read("en").text.find("3 sensors")==std::string::npos)
            throw std::runtime_error("Embedded English model did not recognise the fixture");
        for(auto lang:{"zh-hans","zh-hant"}){
            if(read(lang).text.empty())
                throw std::runtime_error("Embedded Chinese/English model pair did not initialise or recognise text");
        }
        const auto automatic=read("");
        if(automatic.text.find("3 sensors")==std::string::npos)
            throw std::runtime_error("Automatic mode did not recognise the English fixture");
        if(automatic.language_uncertain!=automatic.label.empty())
            throw std::runtime_error("Uncertain language must never display a specific language label");
        if(std::size(image_languages)!=53)
            throw std::runtime_error("PLaMo language roster is incomplete");
        // Every Windows resource must be loadable even if this small test
        // fixture has no meaningful text in that language.
        std::vector<unsigned char> blank(256*128,255);
        for(const auto& lang:image_languages){
            const auto verified=read_page_image({blank,256,128,256,220},lang.tag,{});
            if(verified.label!=lang.label||verified.language_tag!=lang.tag||verified.language_uncertain)
                throw std::runtime_error("Missing language data: "+std::string(lang.tag));
        }
        pdf.page_info(1,1);wait(PdfEvent::Kind::PageInfo);
        if(pdf.render_page_image(1,0,0,43)||!pdf.render_page_image(1,1,0,44))
            throw std::runtime_error("Page navigation left image reading on the wrong page");
        e=wait(PdfEvent::Kind::PageImage);
        if(!e->pixels||e->page!=1||e->image_generation!=44)
            throw std::runtime_error("Subsequent page render was lost");
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
