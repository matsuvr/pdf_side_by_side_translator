#include "pdf/pdf_service.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <cmath>
#include <limits>

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
        if(std::abs(device.x-96)>1||std::abs(device.y-112)>1)throw std::runtime_error("PDF point does not match independently authored fixture pixels");
        const auto restored=view->transform.page_px_to_pdf.apply(device);
        if(std::abs(restored.x-48)>1e-8||std::abs(restored.y-736)>1e-8)throw std::runtime_error("Coordinate round trip failed");
        auto tile=wait(opt::PdfEvent::Kind::TileReady,1);
        if(tile->key.flags!=opt::pdf_render_flags||view->key.flags!=opt::pdf_render_flags)throw std::runtime_error("Worker and tile identity lost limited PDFium image caching");
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
        if(!extraction->layout||extraction->layout->glyphs.size()!=222)throw std::runtime_error("Missing glyph records");
        service.search({1,0,4,"NETWORK",0,false,false,extraction->layout});
        auto search=wait(opt::PdfEvent::Kind::SearchResult,1);
        if(search->query_generation!=4||!search->hit||search->hit->source_ranges.empty())throw std::runtime_error("Search lost its query generation or original glyph mapping");
        service.search({1,0,5,"NETWORK",0,true,false,extraction->layout});
        if(wait(opt::PdfEvent::Kind::SearchResult,1)->hit)throw std::runtime_error("Case-sensitive PDF search was ignored");
        service.search({1,0,6,"the",0,false,false,extraction->layout});
        const auto forward=wait(opt::PdfEvent::Kind::SearchResult,1);
        service.search({1,0,7,"the",std::numeric_limits<std::size_t>::max(),false,true,extraction->layout});
        const auto backward=wait(opt::PdfEvent::Kind::SearchResult,1);
        if(!forward->hit||!backward->hit||forward->hit->begin>=backward->hit->begin)throw std::runtime_error("Previous search did not traverse reading order backwards");
        for(unsigned i=100;i<=163;++i)service.search({1,0,i,i==163?"source":"nonexistent",0,false,false,extraction->layout});
        do {search=wait(opt::PdfEvent::Kind::SearchResult,1);}while(search->query_generation!=163);
        if(!search->hit)throw std::runtime_error("Latest query was lost behind obsolete searches");
        // Replace in-flight work and open the same file as a new document epoch.
        for(int i=0;i<100;++i)service.viewport({1,1,2000+i,2400+i,i%4,{{0,0}}});
        service.open(2,std::filesystem::u8path(argv[1]));
        wait(opt::PdfEvent::Kind::Opened,2);
        for(int r=0;r<4;++r){
            service.viewport({2,0,r%2?1584:1224,r%2?1224:1584,r,{{0,0}}});
            auto current=wait(opt::PdfEvent::Kind::ViewReady,2);
            auto p=current->transform.pdf_to_page_px.apply({48,736});
            const opt::PdfPoint expected[]{{96,112},{1472,96},{1128,1472},{112,1128}};
            if(std::hypot(p.x-expected[r].x,p.y-expected[r].y)>1)throw std::runtime_error("User rotation disagrees with fixture geometry");
            auto q=current->transform.page_px_to_pdf.apply(p);
            if(std::hypot(q.x-48,q.y-736)>1e-7)throw std::runtime_error("Rotated mapping failed");
            wait(opt::PdfEvent::Kind::TileReady,2);
        }
        service.extract(2,2);
        if(!wait(opt::PdfEvent::Kind::Extracted,2)->layout->glyphs.empty())throw std::runtime_error("Blank page produced text");
        service.close(3);wait(opt::PdfEvent::Kind::Closed,3);
        const auto fixture_directory=std::filesystem::u8path(argv[1]).parent_path();
        service.open(4,fixture_directory/"P04-crop.pdf");
        auto cropped=wait(opt::PdfEvent::Kind::Opened,4);
        if(cropped->info.width_points!=400||cropped->info.height_points!=600)throw std::runtime_error("CropBox dimensions were ignored");
        service.viewport({4,0,800,1200,0,{{0,0}}});
        auto crop_view=wait(opt::PdfEvent::Kind::ViewReady,4);
        const auto crop_point=crop_view->transform.pdf_to_page_px.apply({150,640});
        if(std::hypot(crop_point.x-100,crop_point.y-120)>1)throw std::runtime_error("Nonzero CropBox origin displaced geometry");
        service.open(5,fixture_directory/"P05-rotation.pdf");wait(opt::PdfEvent::Kind::Opened,5);
        service.page_info(5,1);
        const auto intrinsic=wait(opt::PdfEvent::Kind::PageInfo,5);
        if(intrinsic->info.width_points!=792||intrinsic->info.height_points!=612)throw std::runtime_error("Intrinsic Rotate dimensions were applied twice");
        service.viewport({5,1,1224,1584,1,{{0,0}}});
        const auto combined=wait(opt::PdfEvent::Kind::ViewReady,5)->transform.pdf_to_page_px.apply({48,736});
        if(std::hypot(combined.x-1128,combined.y-1472)>1)throw std::runtime_error("Intrinsic and user rotation do not compose correctly");
        service.stop();service.join();
        if(!service.done()||wrong_thread)throw std::runtime_error("PDF thread ownership or shutdown failed");
        opt::LayoutOptions small;small.output_bytes=1024;
        opt::PdfService constrained([&](auto event){std::lock_guard lock(mutex);events.push_back(std::move(event));cv.notify_one();},{},small);
        constrained.open(6,std::filesystem::u8path(argv[1]));wait(opt::PdfEvent::Kind::Opened,6);
        constrained.extract(6,0);auto limited=wait(opt::PdfEvent::Kind::Extracted,6);
        if(!limited->layout||limited->layout->availability!=opt::PageLayout::Availability::ResourceLimited||!limited->layout->glyphs.empty())throw std::runtime_error("Over-budget text did not produce a small, editable result");
        constrained.viewport({6,0,612,792,0,{{0,0}}});wait(opt::PdfEvent::Kind::TileReady,6);
        constrained.stop();constrained.join();
        std::cout<<"Async PDF ownership, bounded tiles, replacement, rotation and extraction passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
