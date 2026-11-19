#include "ocr/image_reading.hpp"
#include "pdf/pdf_service.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace opt {
bool read_bundled_image_model(const char*,std::vector<char>*);
}

int main(int argc,char** argv){
    if(argc!=2)return 2;
    using namespace opt;
    using namespace std::chrono_literals;
    try{
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
        const auto read=[&](ImageLanguage language,std::stop_token stop={}){
            return read_page_image(p.bgra.data(),p.width,p.height,p.stride,e->image_dpi,
                                   language,stop,read_bundled_image_model);
        };
        std::stop_source canceled;canceled.request_stop();
        if(!read(ImageLanguage::English,canceled.get_token()).empty())
            throw std::runtime_error("Canceled image request produced text");
        if(read(ImageLanguage::English).find("3 sensors")==std::string::npos)
            throw std::runtime_error("Embedded English model did not recognise the fixture");
        for(auto lang:{ImageLanguage::SimplifiedChinese,ImageLanguage::TraditionalChinese}){
            if(read(lang).empty())
                throw std::runtime_error("Embedded Chinese/English model pair did not initialise or recognise text");
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
