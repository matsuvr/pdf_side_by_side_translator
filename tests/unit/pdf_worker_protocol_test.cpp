#include "pdf/worker_protocol.hpp"
#include "pdf/layout.hpp"
#include <iostream>
#include <stdexcept>
#include <limits>

using namespace opt;
namespace {
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F>void reject(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("Invalid IPC data accepted");}
void put(pdf_wire::Bytes& bytes,std::size_t offset,std::uint64_t value){
    if(offset+8>bytes.size())throw std::runtime_error("Bad test offset");
    for(unsigned i=0;i<8;++i)bytes[offset+i]=static_cast<unsigned char>(value>>(8*i));
}
}
int main(){
    try{
        pdf_wire::Command command;command.op=pdf_wire::Operation::View;command.document=42;command.page=1;
        command.view={42,1,1024,1536,0,{{0,0},{512,0}}};
        auto encoded=pdf_wire::encode(command);auto decoded=pdf_wire::decode_command(encoded);
        require(decoded.document==42&&decoded.view.tiles==command.view.tiles,"Viewport command round trip");
        auto bad_count=encoded;put(bad_count,6*8,UINT64_MAX);
        reject([&]{pdf_wire::decode_command(bad_count);});
        auto bad_operation=encoded;put(bad_operation,0,UINT64_MAX);reject([&]{pdf_wire::decode_command(bad_operation);});
        encoded.push_back(0);reject([&]{pdf_wire::decode_command(encoded);});
        for(std::size_t i=0;i<encoded.size()-1;++i)reject([&]{pdf_wire::decode_command(std::span(encoded).first(i));});

        PdfEvent event;event.kind=PdfEvent::Kind::TileReady;event.document=2;event.page=1;
        event.key={2,1,520,515,0,pdf_render_flags,512,512};event.transform={520,515,0,{},{}};
        auto raster=std::make_shared<RasterBuffer>();raster->width=8;raster->height=3;raster->stride=32;raster->bgra.resize(96,255);event.pixels=raster;
        auto tile=pdf_wire::decode_event(pdf_wire::encode(event));
        require(tile.pixels&&tile.pixels->bgra==raster->bgra&&tile.key==event.key,"Tile round trip");
        raster->stride=31;reject([&]{pdf_wire::decode_event(pdf_wire::encode(event));});raster->stride=32;
        event.key.full_width=max_page_pixels+1;reject([&]{pdf_wire::decode_event(pdf_wire::encode(event));});event.key.full_width=520;
        event.key.x=0;reject([&]{pdf_wire::decode_event(pdf_wire::encode(event));});

        event={};event.kind=PdfEvent::Kind::Extracted;event.document=7;event.page=0;
        std::vector<GlyphRecord> glyphs;
        for(std::size_t i=0;i<5;++i){GlyphRecord g;g.index=static_cast<PdfCharIndex>(i);g.unicode=U"Hello"[i];g.bounds={10.0+i*10,80,19.0+i*10,90};g.origin={10.0+i*10,80};g.valid_bounds=true;g.font_size=10;glyphs.push_back(g);}
        auto layout=std::make_shared<PageLayout>(LayoutEngine{}.build(0,glyphs,{1,0,0,-1,0,100},100,100));
        event.layout=layout;
        auto result=pdf_wire::decode_event(pdf_wire::encode(event));
        require(result.layout&&result.layout->reading_text_utf8==layout->reading_text_utf8&&result.layout->glyphs.size()==5,"Layout round trip");
        require(!layout->reading_mapping.empty(),"Fixture produced mapping");
        auto correct=pdf_wire::encode(event);
        for(std::size_t i=0;i<correct.size();i+=29)reject([&]{pdf_wire::decode_event(std::span(correct).first(i));});
        auto original=layout->reading_mapping.front();
        layout->reading_mapping.front().utf8_end=UINT64_MAX;reject([&]{pdf_wire::decode_event(pdf_wire::encode(event));});layout->reading_mapping.front()=original;
        layout->reading_mapping.front().source_ranges={{UINT32_MAX,2}};reject([&]{pdf_wire::decode_event(pdf_wire::encode(event));});layout->reading_mapping.front()=original;
        layout->reading_mapping.front().source_ranges.assign(6,{0,5});reject([&]{pdf_wire::decode_event(pdf_wire::encode(event));});layout->reading_mapping.front()=original;
        layout->reading_text_utf8="\xc0\x80";reject([&]{pdf_wire::decode_event(pdf_wire::encode(event));});
        std::cout<<"PDF worker command/event bounds, text mapping and raster validation passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
