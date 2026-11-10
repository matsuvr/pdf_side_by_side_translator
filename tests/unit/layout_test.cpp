#include "pdf/layout.hpp"
#include <iostream>
#include <stdexcept>

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
        std::cout<<"Normalization, source mapping, search, crop and availability checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
