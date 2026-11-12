#include "pdf/layout.hpp"
#include <cstdlib>

// Build small pages with varying coordinates, font sizes, reading directions
// and case. Compare search positions with the standard string operations.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,std::size_t size){
    if(size<6)return 0;
    std::vector<opt::GlyphRecord> glyphs;
    for(std::size_t i=1;i+4<size&&glyphs.size()<128;i+=5){
        const double x=data[i],y=data[i+1],height=1+data[i+2]%24;
        const char32_t c=U' '+data[i+3]%95;
        opt::GlyphRecord g;g.index=static_cast<opt::PdfCharIndex>(glyphs.size());
        g.unicode=c;g.origin={x,y};g.bounds={x,y,x+height/2,y+height};g.valid_bounds=true;
        g.font_size=height;g.angle_radians=(data[i+4]%4)*1.5707963267948966;
        glyphs.push_back(g);
    }
    const auto layout=opt::LayoutEngine{}.build(0,std::move(glyphs),{},300,300);
    const auto& text=layout.reading_text_utf8;
    if(text.empty())return 0;
    const auto at=data[0]%text.size();
    const auto query=text.substr(at,1+data[size-1]%6);
    const auto forward=opt::find_text(layout,query,0,true);
    const auto backward=opt::find_text_backward(layout,query,text.size(),true);
    if(!forward||!backward||forward->begin!=text.find(query)||backward->begin!=text.rfind(query))
        std::abort();
    return 0;
}
