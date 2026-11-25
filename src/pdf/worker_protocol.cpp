#include "pdf/worker_protocol.hpp"
#include "core/utf8.hpp"
#include "platform/clear_secret.hpp"
#include "ocr/image_reading.hpp"
#include <bit>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <algorithm>

namespace opt::pdf_wire {
namespace {
[[noreturn]] void invalid(){throw std::runtime_error("Invalid PDF worker message");}
struct Writer {
    Bytes bytes;
    void raw(std::span<const unsigned char> b){
        if(b.size()>max_frame-bytes.size())invalid();
        bytes.insert(bytes.end(),b.begin(),b.end());
    }
    void u(std::uint64_t v){unsigned char b[8];for(unsigned i=0;i<8;++i)b[i]=static_cast<unsigned char>(v>>(i*8));raw(b);}
    void i(int v){u(static_cast<std::uint32_t>(v));}
    void f(double v){if(!std::isfinite(v))invalid();u(std::bit_cast<std::uint64_t>(v));}
    void flag(bool v){u(v);}
    void str(std::string_view v){u(v.size());raw({reinterpret_cast<const unsigned char*>(v.data()),v.size()});}
    template<class T,class F>void list(const std::vector<T>& v,F fn){u(v.size());for(const auto& x:v)fn(x);}
};
struct Reader {
    std::span<const unsigned char> bytes;
    std::size_t remaining_allocation=32U*1024*1024;
    std::span<const unsigned char> take(std::size_t n){if(n>bytes.size())invalid();auto result=bytes.first(n);bytes=bytes.subspan(n);return result;}
    std::uint64_t u(){auto b=take(8);std::uint64_t n=0;for(unsigned i=0;i<8;++i)n|=static_cast<std::uint64_t>(b[i])<<(i*8);return n;}
    std::uint32_t u32(){const auto n=u();if(n>UINT32_MAX)invalid();return static_cast<std::uint32_t>(n);}
    int i(){return std::bit_cast<std::int32_t>(u32());}
    std::size_t size(){auto n=u();if(n>std::numeric_limits<std::size_t>::max())invalid();return static_cast<std::size_t>(n);}
    double f(){auto n=std::bit_cast<double>(u());if(!std::isfinite(n)||std::abs(n)>1e15)invalid();return n;}
    bool flag(){auto n=u();if(n>1)invalid();return n!=0;}
    void allocate(std::size_t n){if(n>remaining_allocation)invalid();remaining_allocation-=n;}
    std::string str(std::size_t max=32U*1024*1024){auto n=size();if(n>max)invalid();allocate(n);auto b=take(n);std::string result(reinterpret_cast<const char*>(b.data()),n);validate_utf8(result);return result;}
    template<class T,class F>std::vector<T> list(F fn,std::size_t max=1024U*1024){
        auto n=size();if(n>max||n>bytes.size()/8||n>remaining_allocation/sizeof(T))invalid();allocate(n*sizeof(T));
        std::vector<T> result;result.reserve(n);while(n--)result.push_back(fn());return result;
    }
    void done(){if(!bytes.empty())invalid();}
};
void write(Writer& w,const PdfPoint& v){w.f(v.x);w.f(v.y);}
PdfPoint point(Reader& r){return {r.f(),r.f()};}
void write(Writer& w,const PdfRect& v){w.f(v.left);w.f(v.bottom);w.f(v.right);w.f(v.top);}
PdfRect rect(Reader& r){return {r.f(),r.f(),r.f(),r.f()};}
void write(Writer& w,const Affine2D& v){w.f(v.a);w.f(v.b);w.f(v.c);w.f(v.d);w.f(v.tx);w.f(v.ty);}
Affine2D affine(Reader& r){return {r.f(),r.f(),r.f(),r.f(),r.f(),r.f()};}
void write(Writer& w,const PdfPageInfo& v){w.f(v.width_points);w.f(v.height_points);write(w,v.pdf_to_layout);w.f(v.layout_width);w.f(v.layout_height);}
PdfPageInfo info(Reader& r){return {r.f(),r.f(),affine(r),r.f(),r.f()};}
void write(Writer& w,const TileKey& v){w.u(v.document);w.u(v.page);w.i(v.full_width);w.i(v.full_height);w.i(v.rotation);w.i(v.flags);w.i(v.x);w.i(v.y);}
TileKey key(Reader& r){return {r.u(),r.u32(),r.i(),r.i(),r.i(),r.i(),r.i(),r.i()};}
void write(Writer& w,const RenderTransform& v){w.i(v.full_width_px);w.i(v.full_height_px);w.i(v.user_rotation_quarters);write(w,v.pdf_to_page_px);write(w,v.page_px_to_pdf);}
RenderTransform transform(Reader& r){return {r.i(),r.i(),r.i(),affine(r),affine(r)};}
void write(Writer& w,const SourceCharRange& v){w.u(v.first);w.u(v.count);}
SourceCharRange range(Reader& r){return {r.u32(),r.u32()};}
void write(Writer& w,const TextMapping& v){w.u(v.utf8_begin);w.u(v.utf8_end);w.list(v.source_ranges,[&](auto& x){write(w,x);});}
TextMapping mapping(Reader& r){return {r.size(),r.size(),r.list<SourceCharRange>([&]{return range(r);})};}
void write(Writer& w,const TextLine& v){w.u(v.utf8_begin);w.u(v.utf8_end);write(w,v.source_bounds);}
TextLine line(Reader& r){return {r.size(),r.size(),rect(r)};}
void write(Writer& w,const GlyphRecord& v){
    // Invalid glyph geometry is explicitly unavailable, never transferred as NaN.
    const bool finite=v.origin.finite()&&std::isfinite(v.font_size)&&std::isfinite(v.angle_radians)&&std::isfinite(v.bounds.left)&&std::isfinite(v.bounds.right)&&std::isfinite(v.bounds.top)&&std::isfinite(v.bounds.bottom);
    w.u(v.index);w.u(v.unicode);write(w,finite?v.bounds:PdfRect{});write(w,finite?v.origin:PdfPoint{});
    w.f(finite?v.font_size:0);w.f(finite?v.angle_radians:0);w.u(v.font_id);w.flag(v.generated);w.flag(v.valid_bounds&&finite);
    w.flag(v.unicode_map_error);w.u(v.source_count);w.flag(v.unicode_continuation);
}
GlyphRecord glyph(Reader& r){
    GlyphRecord v;v.index=r.u32();v.unicode=r.u32();v.bounds=rect(r);v.origin=point(r);v.font_size=r.f();v.angle_radians=r.f();v.font_id=r.u32();
    v.generated=r.flag();v.valid_bounds=r.flag();v.unicode_map_error=r.flag();v.source_count=r.u32();v.unicode_continuation=r.flag();
    if(v.unicode>0x10ffff||v.source_count<1||v.source_count>2)invalid();return v;
}
void write(Writer& w,const TextBlock& v){
    w.str(v.block_id);w.u(v.page);w.u(static_cast<unsigned>(v.kind));w.u(v.reading_order);w.str(v.reading_text_utf8);w.str(v.translation_input_utf8);
    w.list(v.reading_mapping,[&](auto& x){write(w,x);});w.list(v.translation_mapping,[&](auto& x){write(w,x);});
    w.list(v.source_rects,[&](auto& x){write(w,x);});w.list(v.lines,[&](auto& x){write(w,x);});w.f(v.layout_confidence);
}
TextBlock block(Reader& r){
    TextBlock v;v.block_id=r.str(4096);v.page=r.u32();auto kind=r.u32();if(kind>static_cast<unsigned>(BlockKind::Unknown))invalid();v.kind=static_cast<BlockKind>(kind);v.reading_order=r.u32();
    v.reading_text_utf8=r.str();v.translation_input_utf8=r.str();v.reading_mapping=r.list<TextMapping>([&]{return mapping(r);});v.translation_mapping=r.list<TextMapping>([&]{return mapping(r);});
    v.source_rects=r.list<PdfRect>([&]{return rect(r);});v.lines=r.list<TextLine>([&]{return line(r);});const auto confidence=r.f();if(confidence<0||confidence>1)invalid();v.layout_confidence=static_cast<float>(confidence);return v;
}
std::size_t check_ranges(const std::vector<SourceCharRange>& ranges,std::size_t glyphs){
    std::size_t total=0;for(auto v:ranges){
        if(!v.count||v.first>=glyphs||v.count>glyphs-v.first||v.count>glyphs*4-total)invalid();
        total+=v.count;
    }return total;
}
void check_position(std::string_view text,std::size_t lo,std::size_t hi){
    auto boundary=[&](std::size_t n){return n==text.size()||(n<text.size()&&(static_cast<unsigned char>(text[n])&0xc0)!=0x80);};
    if(lo>hi||hi>text.size()||!boundary(lo)||!boundary(hi))invalid();
}
std::size_t validate_mappings(const std::vector<TextMapping>& mappings,std::string_view text,std::size_t glyphs){
    std::size_t last=0,total=0;for(auto& m:mappings){
        check_position(text,m.utf8_begin,m.utf8_end);if(m.utf8_begin>=m.utf8_end||m.utf8_begin<last)invalid();last=m.utf8_end;
        const auto count=check_ranges(m.source_ranges,glyphs);if(count>glyphs*4-total)invalid();total+=count;
    }return total;
}
void validate_lines(const std::vector<TextLine>& lines,std::string_view text){for(auto& l:lines)check_position(text,l.utf8_begin,l.utf8_end);}
void write(Writer& w,const PageLayout& v){
    w.u(v.page);w.u(v.layout_version);w.u(static_cast<unsigned>(v.availability));write(w,v.pdf_to_layout);
    w.list(v.glyphs,[&](auto& x){write(w,x);});w.list(v.blocks,[&](auto& x){write(w,x);});w.str(v.reading_text_utf8);
    w.list(v.reading_mapping,[&](auto& x){write(w,x);});w.list(v.lines,[&](auto& x){write(w,x);});
}
std::shared_ptr<const PageLayout> layout(Reader& r){
    auto v=std::make_shared<PageLayout>();v->page=r.u32();v->layout_version=r.u32();if(v->layout_version!=PageLayout::current_version)invalid();
    auto a=r.u32();if(a>static_cast<unsigned>(PageLayout::Availability::ResourceLimited))invalid();v->availability=static_cast<PageLayout::Availability>(a);
    v->pdf_to_layout=affine(r);v->glyphs=r.list<GlyphRecord>([&]{return glyph(r);});
    const auto n=v->glyphs.size();for(std::size_t i=0;i<n;++i){auto& g=v->glyphs[i];if(g.index!=i||g.source_count>n-i)invalid();}
    v->blocks=r.list<TextBlock>([&]{return block(r);});v->reading_text_utf8=r.str();v->reading_mapping=r.list<TextMapping>([&]{return mapping(r);});v->lines=r.list<TextLine>([&]{return line(r);});
    validate_mappings(v->reading_mapping,v->reading_text_utf8,n);validate_lines(v->lines,v->reading_text_utf8);
    std::size_t total=0;
    for(const auto& b:v->blocks){
        if(b.page!=v->page)invalid();const auto mapped=validate_mappings(b.reading_mapping,b.reading_text_utf8,n)+validate_mappings(b.translation_mapping,b.translation_input_utf8,n);
        if(mapped>n*8-total)invalid();total+=mapped;validate_lines(b.lines,b.reading_text_utf8);
    }
    if(layout_memory_bytes(*v)>32U*1024*1024)invalid();return v;
}
void validate_info(const PdfPageInfo& v){
    if(v.width_points<=0||v.height_points<=0||v.layout_width<=0||v.layout_height<=0||v.layout_width>max_page_pixels||v.layout_height>max_page_pixels)invalid();
}
void validate_tile(const PdfEvent& v){
    const auto& k=v.key;const auto& t=v.transform;
    if(k.document!=v.document||k.page!=v.page||k.flags!=pdf_render_flags||k.full_width<=0||k.full_height<=0||k.full_width>max_page_pixels||k.full_height>max_page_pixels||
       k.rotation<0||k.rotation>3||t.full_width_px!=k.full_width||t.full_height_px!=k.full_height||t.user_rotation_quarters!=k.rotation||
       k.x<0||k.y<0||k.x>=k.full_width||k.y>=k.full_height||k.x%512||k.y%512)invalid();
}
void validate_raster(const PdfEvent& v,const RasterBuffer& p,std::size_t n){
    if(p.width<=0||p.height<=0||p.stride<=0)invalid();
    if(v.kind==PdfEvent::Kind::TileReady){
        validate_tile(v);if(p.width!=std::min(512,v.key.full_width-v.key.x)||p.height!=std::min(512,v.key.full_height-v.key.y)||p.stride!=p.width*4)invalid();
    }else if(v.kind==PdfEvent::Kind::PageImage){
        if(p.width>image_reading_max_side||p.height>image_reading_max_side||p.stride!=((p.width+3)&~3)||v.image_dpi<=0||v.image_dpi>image_reading_dpi)invalid();
    }else invalid();
    if(n!=static_cast<std::size_t>(p.stride)*p.height||n>static_cast<std::size_t>(image_reading_max_area))invalid();
}
}
Command::~Command(){clear_secret(password);}
Bytes encode(const Command& v){
    Writer w;w.u(static_cast<unsigned>(v.op));w.u(v.document);w.u(v.page);
    switch(v.op){
    case Operation::Open:w.str(v.password);break;
    case Operation::Page:case Operation::Extract:break;
    case Operation::View:w.i(v.view.width);w.i(v.view.height);w.i(v.view.rotation);w.list(v.view.tiles,[&](auto& p){w.i(p.first);w.i(p.second);});break;
    case Operation::Image:w.i(v.rotation);w.u(v.generation);break;
    case Operation::Cancel:w.u(v.generation);break;
    case Operation::Search:w.u(v.search.generation);w.str(v.search.query);w.u(v.search.start);w.flag(v.search.match_case);w.flag(v.search.backward);break;
    }
    if(w.bytes.size()>max_command)invalid();return std::move(w.bytes);
}
Command decode_command(std::span<const unsigned char> bytes){
    if(bytes.size()>max_command)invalid();Reader r{bytes};Command v;const auto op=r.u32();if(op>static_cast<unsigned>(Operation::Cancel))invalid();
    v.op=static_cast<Operation>(op);v.document=r.u();v.page=r.u32();
    switch(v.op){
    case Operation::Open:v.password=r.str(4096);break;
    case Operation::Page:case Operation::Extract:break;
    case Operation::View:v.view.document=v.document;v.view.page=v.page;v.view.width=r.i();v.view.height=r.i();v.view.rotation=r.i();v.view.tiles=r.list<std::pair<int,int>>([&]{return std::pair{r.i(),r.i()};},64);break;
    case Operation::Image:v.rotation=r.i();v.generation=r.u();break;
    case Operation::Cancel:v.generation=r.u();break;
    case Operation::Search:v.search.document=v.document;v.search.page=v.page;v.search.generation=r.u();v.search.query=r.str(32768);v.search.start=r.size();v.search.match_case=r.flag();v.search.backward=r.flag();break;
    }
    r.done();return v;
}
Bytes encode(const PdfEvent& v){
    Writer w;w.u(static_cast<unsigned>(v.kind));w.u(v.document);w.u(v.page);w.u(v.page_count);w.flag(v.has_page_info);write(w,v.info);write(w,v.key);write(w,v.transform);
    w.u(v.query_generation);w.u(v.image_generation);w.i(v.image_dpi);
    w.str(v.code);w.str(v.message);w.str(v.detail);w.str(v.document_language);
    w.flag(bool(v.pixels));if(v.pixels){w.i(v.pixels->width);w.i(v.pixels->height);w.i(v.pixels->stride);w.u(v.pixels->bgra.size());w.raw(v.pixels->bgra);}
    w.flag(bool(v.layout));if(v.layout)write(w,*v.layout);
    w.flag(bool(v.hit));if(v.hit){w.u(v.hit->begin);w.u(v.hit->end);w.list(v.hit->source_ranges,[&](auto& x){write(w,x);});}
    return std::move(w.bytes);
}
PdfEvent decode_event(std::span<const unsigned char> bytes){
    if(bytes.size()>max_frame)invalid();Reader r{bytes};PdfEvent v;const auto kind=r.u32();if(kind>static_cast<unsigned>(PdfEvent::Kind::Failed))invalid();v.kind=static_cast<PdfEvent::Kind>(kind);
    v.document=r.u();v.page=r.u32();v.page_count=r.u32();v.has_page_info=r.flag();v.info=info(r);v.key=key(r);v.transform=transform(r);v.query_generation=r.u();v.image_generation=r.u();v.image_dpi=r.i();
    v.code=r.str(128);v.message=r.str(4096);v.detail=r.str(4096);v.document_language=r.str(128);
    if(r.flag()){
        auto p=std::make_shared<RasterBuffer>();p->width=r.i();p->height=r.i();p->stride=r.i();auto n=r.size();validate_raster(v,*p,n);r.allocate(n);auto b=r.take(n);p->bgra.assign(b.begin(),b.end());v.pixels=std::move(p);
    }
    if(r.flag()){v.layout=layout(r);if(v.layout->page!=v.page||(v.kind!=PdfEvent::Kind::Extracted&&v.kind!=PdfEvent::Kind::SearchResult))invalid();}
    if(r.flag()){
        if(v.kind!=PdfEvent::Kind::SearchResult||!v.layout)invalid();TextHit h;h.begin=r.size();h.end=r.size();h.source_ranges=r.list<SourceCharRange>([&]{return range(r);});
        check_position(v.layout->reading_text_utf8,h.begin,h.end);check_ranges(h.source_ranges,v.layout->glyphs.size());v.hit=std::move(h);
    }
    if(v.kind==PdfEvent::Kind::Opened){if(!v.page_count||v.page_count>static_cast<unsigned>(std::numeric_limits<int>::max()))invalid();if(v.has_page_info)validate_info(v.info);}
    if(v.kind==PdfEvent::Kind::PageInfo)validate_info(v.info);
    if(v.kind==PdfEvent::Kind::ViewReady)validate_tile(v);
    if(v.kind==PdfEvent::Kind::TileReady&&!v.pixels)invalid();
    if((v.kind==PdfEvent::Kind::Extracted||v.kind==PdfEvent::Kind::SearchResult)&&!v.layout)invalid();
    r.done();return v;
}
}
