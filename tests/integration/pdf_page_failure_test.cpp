// A damaged first page must not make the rest of the document unopenable.
#include "pdf/pdf_service.hpp"
#include "platform/digest.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>

using namespace std::chrono_literals;
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::string make_pdf(){
    std::string pdf="%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";
    std::size_t offsets[5]{};
    auto object=[&](int number,const std::string& body){
        offsets[number]=pdf.size();
        pdf+=std::to_string(number)+" 0 obj\n"+body+"\nendobj\n";
    };
    object(1,"<</Type/Catalog/Pages 2 0 R>>");
    object(2,"<</Type/Pages/Kids[3 0 R 4 0 R]/Count 2>>");
    object(3,"null");
    object(4,"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Resources<<>>>>");
    const auto xref=pdf.size();
    pdf+="xref\n0 5\n0000000000 65535 f \n";
    for(int i=1;i<=4;++i){
        char entry[32]{};
        std::snprintf(entry,sizeof(entry),"%010zu 00000 n \n",offsets[i]);
        pdf+=entry;
    }
    pdf+="trailer\n<</Size 5/Root 1 0 R>>\nstartxref\n"+std::to_string(xref)+"\n%%EOF\n";
    return pdf;
}
}
int main(){
    const auto path=std::filesystem::temp_directory_path()/std::filesystem::path("opt-page-failure-"+opt::new_uuid()+".pdf");
    try{
        {std::ofstream output(path,std::ios::binary);output<<make_pdf();if(!output)throw std::runtime_error("Cannot write fixture");}
        std::mutex mutex;std::condition_variable cv;std::deque<std::shared_ptr<const opt::PdfEvent>> events;
        opt::PdfService service([&](auto event){std::lock_guard lock(mutex);events.push_back(std::move(event));cv.notify_one();});
        auto wait=[&](opt::PdfEvent::Kind kind){
            const auto deadline=std::chrono::steady_clock::now()+10s;
            std::unique_lock lock(mutex);
            for(;;){
                while(!events.empty()){
                    auto event=events.front();events.pop_front();
                    if(event->kind==opt::PdfEvent::Kind::Failed)throw std::runtime_error(event->message);
                    if(event->kind==kind)return event;
                }
                if(cv.wait_until(lock,deadline)==std::cv_status::timeout)throw std::runtime_error("PDF event timeout");
            }
        };
        service.open(1,path);
        auto opened=wait(opt::PdfEvent::Kind::Opened);
        require(opened->page_count==2,"page count lost after a broken first page");
        require(!opened->has_page_info&&!opened->message.empty(),"broken first page reported a valid page");
        service.page_info(1,1);
        auto info=wait(opt::PdfEvent::Kind::PageInfo);
        require(info->has_page_info&&info->info.width_points==612,"second page was not usable");
        opened.reset();info.reset();
        service.stop();service.join();
        std::filesystem::remove(path);
        std::cout<<"broken first page kept the document open and the next page usable\n";
    }catch(const std::exception& error){
        std::error_code ignored;std::filesystem::remove(path,ignored);
        std::cerr<<error.what()<<'\n';return 1;
    }
}
