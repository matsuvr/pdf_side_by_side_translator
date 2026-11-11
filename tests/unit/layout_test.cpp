#include "pdf/layout.hpp"
#include <iostream>
#include <stdexcept>
#include <limits>

void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int main(){
    try{
        std::vector<opt::GlyphRecord> glyphs;
        double x=10;
        const std::u32string text=U"aﬁne\u00ad e\u0301😀";
        for(char32_t c:text){
            const bool accent=c==0x301;
            const double left=accent?x-12:x,width=c==' '?4:10;
            glyphs.push_back({static_cast<opt::PdfCharIndex>(glyphs.size()),c,{left,80,left+width,92},{left,80},12,0,0,false,true,false});
            if(!accent)x+=width;
        }
        auto result=opt::LayoutEngine{}.build(0,glyphs,{1,0,0,-1,0,100},300,100);
        check(result.reading_text_utf8=="afine é😀","Ligature, soft hyphen, combining mark or emoji changed");
        const auto hit=opt::find_text(result,"FI",0,false);
        check(hit&&hit->source_ranges.size()==1&&hit->source_ranges[0].first==1&&hit->source_ranges[0].count==1,"Ligature search lost original glyph mapping");
        check(!opt::find_text(result,"FI",0,true),"Case-sensitive search was ignored");
        check(opt::near_text(result,{15,85})&&!opt::near_text(result,{250,20}),"An image/blank region could start a text selection");
        check(opt::nearest_text_position(result,{11,85})==0,"Line hit test did not map to reading-text start");
        auto same=opt::LayoutEngine{}.build(0,glyphs,{1,0,0,-1,0,100},300,100);
        check(same.blocks[0].block_id==result.blocks[0].block_id,"Block identity is unstable");
        glyphs[0].bounds={-100,80,-90,92};glyphs[0].origin={-100,80};
        auto crop=opt::LayoutEngine{}.build(0,glyphs,{1,0,0,-1,0,100},300,100);
        check(!crop.reading_text_utf8.starts_with("a"),"Crop-excluded character leaked into reading text");
        std::vector<opt::GlyphRecord> unknown{{0,0,{10,80,20,92},{10,80},12,0,0,false,true,true}};
        auto unavailable=opt::LayoutEngine{}.build(0,unknown,{1,0,0,-1,0,100},300,100);
        check(unavailable.availability==opt::PageLayout::Availability::UnicodeUnavailable,"Unknown Unicode was labelled as an image-only page");
        unknown[0].unicode=' ';unknown[0].unicode_map_error=false;
        auto whitespace=opt::LayoutEngine{}.build(0,unknown,{1,0,0,-1,0,100},300,100);
        check(whitespace.availability==opt::PageLayout::Availability::NoTranslatableText,"Whitespace was sent for translation");
        std::vector<opt::GlyphRecord> pieces{{0,'f',{10,80,25,92},{10,80},12,0,1,false,true,false},{1,'i',{10,80,25,92},{10,80},12,0,1,false,true,false}};
        auto piece_layout=opt::LayoutEngine{}.build(0,pieces,{1,0,0,-1,0,100},300,100);
        check(piece_layout.reading_text_utf8=="fi"&&piece_layout.reading_mapping.size()==1&&piece_layout.reading_mapping[0].source_ranges[0].count==2,"PDFium's co-located ligature pieces were not one selectable glyph");
        check(opt::nearest_text_position(piece_layout,{24,85})==2,"Selecting a ligature's right edge loses part of its text");
        pieces[0].bounds.left=std::numeric_limits<double>::quiet_NaN();pieces.resize(1);
        auto nonfinite=opt::LayoutEngine{}.build(0,pieces,{1,0,0,-1,0,100},300,100);check(nonfinite.reading_text_utf8.empty(),"Non-finite glyph geometry reached sorting/hit testing");
        bool limited=false;
        try{opt::LayoutEngine{}.check_glyph_budget(std::numeric_limits<std::size_t>::max());}
        catch(const opt::LayoutResourceLimit&){limited=true;}
        check(limited,"Huge raw glyph count was not rejected before reserve");
        std::vector<opt::GlyphRecord> many;
        for(unsigned i=0;i<4096;++i){
            const double at=(i+1)*1000.0/4097;
            many.push_back({i,'A',{at,10,at+0.005,10.01},{at,10},0.01,0,0,false,true,false});
        }
        opt::LayoutOptions tight;tight.working_bytes=2*1024*1024;tight.output_bytes=32*1024*1024;
        limited=false;
        try{opt::LayoutEngine(tight).build(0,many,{},1024,1024);}catch(const opt::LayoutResourceLimit&){limited=true;}
        check(limited,"Intermediate layout allocations ignored the working budget");
        tight.working_bytes=128*1024*1024;tight.output_bytes=1024*1024;
        limited=false;
        try{opt::LayoutEngine(tight).build(0,many,{},1024,1024);}catch(const opt::LayoutResourceLimit&){limited=true;}
        check(limited,"Output mapping allocations ignored the layout budget");
        std::size_t checkpoints=0;
        auto fragmented=opt::LayoutEngine{}.build(0,many,{},1024,1024,[&]{++checkpoints;return false;});
        check(fragmented.glyphs.size()==many.size()&&opt::layout_memory_bytes(fragmented)<=opt::LayoutOptions{}.output_bytes,"Fragmented page exceeded its final layout budget");
        check(checkpoints>20,"Large layout has too few cancellation checkpoints");
        std::size_t polls=0;bool cancelled=false;
        try{opt::LayoutEngine{}.build(0,many,{},1024,1024,[&]{return ++polls>=checkpoints*3/4;});}
        catch(const opt::LayoutCancelled&){cancelled=true;}
        check(cancelled,"Cancellation during late layout construction was ignored");
        cancelled=false;
        try{opt::LayoutEngine{}.build(0,many,{},1024,1024,[]{return true;});}
        catch(const opt::LayoutCancelled&){cancelled=true;}
        check(cancelled,"Already-cancelled extraction performed layout work");
        std::cout<<"Normalization, source mapping, search, crop and availability checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
