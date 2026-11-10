#include "pdf/pdf_service.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>

void json_string(std::string_view text){
    std::cout<<'"';
    constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:text){
        if(c=='"'||c=='\\')std::cout<<'\\'<<static_cast<char>(c);
        else if(c<32)std::cout<<"\\u00"<<hex[c>>4]<<hex[c&15];
        else std::cout<<static_cast<char>(c);
    }
    std::cout<<'"';
}
int main(int argc,char** argv){
    if(argc<2||argc>3)return 2;
    const bool mappings=argc==3&&std::string(argv[2])=="--mappings";
    try{
        std::mutex mutex;std::condition_variable cv;std::deque<std::shared_ptr<const opt::PdfEvent>> events;
        opt::PdfService pdf([&](auto e){std::lock_guard lock(mutex);events.push_back(e);cv.notify_one();});
        auto wait=[&](opt::PdfEvent::Kind kind){
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            std::unique_lock lock(mutex);
            while(true){
                while(!events.empty()){
                    auto event=events.front();events.pop_front();
                    if(event->kind==opt::PdfEvent::Kind::Failed)throw std::runtime_error(event->message);
                    if(event->kind==kind)return event;
                }
                if(cv.wait_until(lock,deadline)==std::cv_status::timeout)throw std::runtime_error("PDF extraction timeout");
            }
        };
        pdf.open(1,std::filesystem::u8path(argv[1]));const auto opened=wait(opt::PdfEvent::Kind::Opened);
        std::cout<<"{\"pages\":[";
        for(opt::PageIndex page=0;page<opened->page_count;++page){
            pdf.extract(1,page);const auto event=wait(opt::PdfEvent::Kind::Extracted);const auto& layout=*event->layout;
            if(page)std::cout<<',';
            std::cout<<"{\"text\":";json_string(layout.reading_text_utf8);
            std::cout<<",\"glyphs\":"<<layout.glyphs.size()<<",\"availability\":"<<static_cast<int>(layout.availability)<<",\"blocks\":[";
            for(std::size_t i=0;i<layout.blocks.size();++i){
                if(i)std::cout<<',';const auto& b=layout.blocks[i];
                std::cout<<"{\"text\":";json_string(b.reading_text_utf8);std::cout<<",\"kind\":"<<static_cast<int>(b.kind)<<'}';
            }
            std::cout<<']';
            if(mappings){
                std::cout<<",\"mapping\":[";
                for(std::size_t i=0;i<layout.reading_mapping.size();++i){const auto& m=layout.reading_mapping[i];if(i)std::cout<<',';
                    std::cout<<"{\"begin\":"<<m.utf8_begin<<",\"end\":"<<m.utf8_end<<",\"ranges\":[";
                    for(std::size_t j=0;j<m.source_ranges.size();++j){if(j)std::cout<<',';const auto r=m.source_ranges[j];std::cout<<'['<<r.first<<','<<r.count<<']';}std::cout<<"]}";
                }std::cout<<']';
            }
            std::cout<<'}';
        }
        std::cout<<"]}\n";pdf.stop();pdf.join();
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
