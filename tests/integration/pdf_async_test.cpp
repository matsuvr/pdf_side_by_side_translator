#include "pdf/pdf_service.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <cmath>

using namespace std::chrono_literals;
int main(int argc,char** argv){
    if(argc!=2)return 2;
    try {
        std::mutex mutex;std::condition_variable cv;
        std::deque<std::shared_ptr<const opt::PdfEvent>> events;
        const auto main_thread=std::this_thread::get_id();
        bool wrong_thread=false;
        opt::PdfService service([&](auto event){
            std::lock_guard lock(mutex);
            if(std::this_thread::get_id()==main_thread)wrong_thread=true;
            events.push_back(event);cv.notify_one();
        });
        auto wait=[&](opt::PdfEvent::Kind kind,opt::DocumentEpoch epoch){
            const auto limit=std::chrono::steady_clock::now()+10s;
            std::unique_lock lock(mutex);
            while(true){
                while(!events.empty()){
                    auto e=events.front();events.pop_front();
                    if(e->document==epoch&&e->kind==opt::PdfEvent::Kind::Failed)throw std::runtime_error(e->message);
                    if(e->kind==kind&&e->document==epoch)return e;
                }
                if(cv.wait_until(lock,limit)==std::cv_status::timeout)throw std::runtime_error("Timed out waiting for PDF worker");
            }
        };
        service.open(1,std::filesystem::u8path(argv[1]));
        auto opened=wait(opt::PdfEvent::Kind::Opened,1);
        if(opened->page_count!=3||opened->info.width_points!=612)throw std::runtime_error("Wrong PDF metadata");
        service.viewport({1,0,1224,1584,0,{{0,0},{512,0}}});
        auto view=wait(opt::PdfEvent::Kind::ViewReady,1);
        const auto device=view->transform.pdf_to_page_px.apply({48,736});
        const auto restored=view->transform.page_px_to_pdf.apply(device);
        if(std::abs(restored.x-48)>1e-8||std::abs(restored.y-736)>1e-8)throw std::runtime_error("Coordinate round trip failed");
        auto tile=wait(opt::PdfEvent::Kind::TileReady,1);
        if(!tile->pixels||tile->pixels->width!=512||tile->pixels->height!=512||tile->pixels->bgra.size()!=512*512*4)
            throw std::runtime_error("Unbounded or malformed tile");
        bool dark=false;
        for(std::size_t i=0;i<tile->pixels->bgra.size();i+=4){
            if(tile->pixels->bgra[i+3]!=255)throw std::runtime_error("Nonopaque PDF raster");
            dark|=tile->pixels->bgra[i]<200;
        }
        if(!dark)throw std::runtime_error("Missing visible text");
        service.extract(1,0);
        auto extraction=wait(opt::PdfEvent::Kind::Extracted,1);
        if(!extraction->glyphs||extraction->glyphs->size()!=222)throw std::runtime_error("Missing glyph records");
        // Replace in-flight work and open the same file as a new document epoch.
        for(int i=0;i<100;++i)service.viewport({1,1,2000+i,2400+i,i%4,{{0,0}}});
        service.open(2,std::filesystem::u8path(argv[1]));
        wait(opt::PdfEvent::Kind::Opened,2);
        for(int r=0;r<4;++r){
            service.viewport({2,0,r%2?1584:1224,r%2?1224:1584,r,{{0,0}}});
            auto current=wait(opt::PdfEvent::Kind::ViewReady,2);
            auto p=current->transform.pdf_to_page_px.apply({48,736});
            auto q=current->transform.page_px_to_pdf.apply(p);
            if(std::hypot(q.x-48,q.y-736)>1e-7)throw std::runtime_error("Rotated mapping failed");
            wait(opt::PdfEvent::Kind::TileReady,2);
        }
        service.extract(2,2);
        if(!wait(opt::PdfEvent::Kind::Extracted,2)->glyphs->empty())throw std::runtime_error("Blank page produced text");
        service.close(3);wait(opt::PdfEvent::Kind::Closed,3);
        service.stop();service.join();
        if(!service.done()||wrong_thread)throw std::runtime_error("PDF thread ownership or shutdown failed");
        std::cout<<"Async PDF ownership, bounded tiles, replacement, rotation and extraction passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
