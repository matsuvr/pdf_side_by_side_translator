#include "pdf/layout.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>

namespace opt {
namespace {
struct Rect {double left=0,top=0,right=0,bottom=0;};
Rect unite(Rect a,Rect b){return {std::min(a.left,b.left),std::min(a.top,b.top),std::max(a.right,b.right),std::max(a.bottom,b.bottom)};}
double median(std::vector<double> values){
    if(values.empty())return 1;
    auto middle=values.begin()+static_cast<std::ptrdiff_t>(values.size()/2);
    std::nth_element(values.begin(),middle,values.end());return std::max(0.001,*middle);
}
bool space(char32_t c){return c==' '||c=='\r'||c=='\n'||c=='\t'||c==0xa0||c==0x3000;}
std::string normalized(char32_t c){
    switch(c){
    case 0xad:return {};case 0xfb00:return "ff";case 0xfb01:return "fi";case 0xfb02:return "fl";
    case 0xfb03:return "ffi";case 0xfb04:return "ffl";case 0xfb05:case 0xfb06:return "st";
    default:return encode_utf8(c?c:0xfffd);
    }
}
bool wide(char32_t c){return (c>=0x2e80&&c<=0xa4cf)||(c>=0xf900&&c<=0xfaff)||(c>=0x1f000&&c<=0x1ffff);}
bool combining(char32_t c){return (c>=0x300&&c<=0x36f)||(c>=0x1ab0&&c<=0x1aff)||(c>=0x1dc0&&c<=0x1dff);}
struct Item {const GlyphRecord* glyph;Rect box;double baseline,height,sort_x;};
struct Fragment {std::vector<Item> items;Rect box;double baseline=0,height=0;int orientation=0;std::shared_ptr<TextBlock> table;};
using Fragments=std::vector<Fragment>;
Rect bounds(const Fragments& fs){Rect r=fs.front().box;for(const auto& f:fs)r=unite(r,f.box);return r;}
struct Gutter {double left=0,right=0;std::size_t crossing=0;bool valid=false;};
Gutter vertical_gutter(const Fragments& fs,double h,const LayoutOptions& options){
    if(fs.size()<4)return {};
    const auto total=bounds(fs);
    std::vector<std::pair<double,int>> edges;
    for(const auto& f:fs){edges.emplace_back(f.box.left,1);edges.emplace_back(f.box.right,-1);}
    std::sort(edges.begin(),edges.end());
    int occupied=0;Gutter best;
    for(std::size_t i=0;i<edges.size();){
        const double position=edges[i].first;
        while(i<edges.size()&&edges[i].first==position)occupied+=edges[i++].second;
        if(i==edges.size())break;
        const double next=edges[i].first;
        if(next-position<h*options.minimum_gutter||position<total.left+(total.right-total.left)*0.15||
           next>total.right-(total.right-total.left)*0.15||occupied>static_cast<int>(std::max<std::size_t>(1,fs.size()/5)))continue;
        std::size_t left=0,right=0;Rect l{},r{};
        for(const auto& f:fs){
            if(f.box.right<=position){l=left++?unite(l,f.box):f.box;}
            else if(f.box.left>=next){r=right++?unite(r,f.box):f.box;}
        }
        if(left<2||right<2)continue;
        const double overlap=std::min(l.bottom,r.bottom)-std::max(l.top,r.top);
        if(overlap<std::min(l.bottom-l.top,r.bottom-r.top)*0.4)continue;
        if(!best.valid||static_cast<std::size_t>(occupied)<best.crossing||
           (static_cast<std::size_t>(occupied)==best.crossing&&next-position>best.right-best.left))
            best={position,next,static_cast<std::size_t>(occupied),true};
    }
    return best;
}
std::optional<double> horizontal_cut(const Fragments& fs,double h,const LayoutOptions& options){
    std::vector<std::pair<double,double>> bands;
    for(const auto& f:fs)bands.emplace_back(f.box.top,f.box.bottom);
    std::sort(bands.begin(),bands.end());
    double bottom=bands.front().second,best=options.horizontal_band_gap*h,cut=0;
    for(std::size_t i=1;i<bands.size();++i){
        const auto [a,b]=bands[i];
        if(a-bottom>best){best=a-bottom;cut=(a+bottom)/2;}
        bottom=std::max(bottom,b);
    }
    if(best>options.horizontal_band_gap*h)return cut;
    return {};
}
void regions(Fragments fs,double h,const LayoutOptions& options,unsigned depth,std::vector<Fragments>& out){
    if(fs.empty())return;
    if(depth<options.maximum_depth&&fs.size()>1){
        if(auto cut=horizontal_cut(fs,h,options)){
            Fragments top,bottom;
            for(auto& f:fs)(f.box.top<*cut?top:bottom).push_back(std::move(f));
            regions(std::move(top),h,options,depth+1,out);regions(std::move(bottom),h,options,depth+1,out);return;
        }
        const auto gutter=vertical_gutter(fs,h,options);
        if(gutter.valid){
            Fragments crossing;
            for(const auto& f:fs)if(f.box.right>gutter.left&&f.box.left<gutter.right)crossing.push_back(f);
            if(crossing.empty()){
                Fragments left,right;
                for(auto& f:fs)(f.box.right<=gutter.left?left:right).push_back(std::move(f));
                regions(std::move(left),h,options,depth+1,out);regions(std::move(right),h,options,depth+1,out);return;
            }
            // Spanning title/caption rows separate local column regions.
            std::sort(crossing.begin(),crossing.end(),[](const auto& a,const auto& b){return a.box.top<b.box.top;});
            std::vector<Rect> bands;
            for(const auto& f:crossing){
                if(bands.empty()||f.box.top>bands.back().bottom+h*0.3)bands.push_back(f.box);
                else bands.back()=unite(bands.back(),f.box);
            }
            Fragments remaining=std::move(fs);
            for(const auto& band:bands){
                Fragments before,row,after;
                for(auto& f:remaining){
                    if(f.box.bottom<band.top)before.push_back(std::move(f));
                    else if(f.box.top<=band.bottom)row.push_back(std::move(f));
                    else after.push_back(std::move(f));
                }
                regions(std::move(before),h,options,depth+1,out);
                std::sort(row.begin(),row.end(),[](const auto& a,const auto& b){return a.box.left<b.box.left;});
                if(!row.empty())out.push_back(std::move(row));
                remaining=std::move(after);
            }
            regions(std::move(remaining),h,options,depth+1,out);return;
        }
    }
    std::sort(fs.begin(),fs.end(),[](const auto& a,const auto& b){
        if(a.baseline!=b.baseline)return a.baseline<b.baseline;
        return a.box.left<b.box.left;
    });
    out.push_back(std::move(fs));
}
void append(TextBlock& block,std::string_view bytes,std::vector<SourceCharRange> ranges){
    if(bytes.empty())return;
    const auto reading=block.reading_text_utf8.size(),translation=block.translation_input_utf8.size();
    block.reading_text_utf8+=bytes;block.translation_input_utf8+=bytes;
    block.reading_mapping.push_back({reading,reading+bytes.size(),ranges});
    block.translation_mapping.push_back({translation,translation+bytes.size(),std::move(ranges)});
}
bool same_painted_glyph(const GlyphRecord& a,const GlyphRecord& b){
    return b.index==a.index+a.source_count&&a.font_id==b.font_id&&std::hypot(a.origin.x-b.origin.x,a.origin.y-b.origin.y)<0.001&&
        std::abs(a.angle_radians-b.angle_radians)<0.00001&&std::abs(a.bounds.left-b.bounds.left)<0.00001&&std::abs(a.bounds.right-b.bounds.right)<0.00001&&
        std::abs(a.bounds.top-b.bounds.top)<0.00001&&std::abs(a.bounds.bottom-b.bounds.bottom)<0.00001;
}
void append_cluster(TextBlock& block,std::string_view bytes,SourceCharRange range,bool join){
    if(bytes.empty())return;
    if(!join||block.reading_mapping.empty()||block.translation_mapping.empty()){append(block,bytes,{range});return;}
    block.reading_text_utf8+=bytes;block.translation_input_utf8+=bytes;
    auto extend=[&](TextMapping& mapping,std::size_t end){mapping.utf8_end=end;
        if(!mapping.source_ranges.empty()&&mapping.source_ranges.back().first+mapping.source_ranges.back().count==range.first)mapping.source_ranges.back().count+=range.count;
        else mapping.source_ranges.push_back(range);};
    extend(block.reading_mapping.back(),block.reading_text_utf8.size());extend(block.translation_mapping.back(),block.translation_input_utf8.size());
}
void append_fragment(TextBlock& block,const Fragment& fragment,const LayoutOptions& options,
                     const std::vector<GlyphRecord>& raw,bool new_line,bool omit_line_space){
    if(new_line&&!omit_line_space&&!block.reading_text_utf8.empty()&&block.reading_text_utf8.back()!='-')append(block," ",{});
    const auto line_begin=block.reading_text_utf8.size();
    const Item* previous=nullptr;
    for(const auto& item:fragment.items){
        const auto c=item.glyph->unicode;
        if(previous){
            const double gap=item.box.left-previous->box.right;
            bool adjacent=false;
            std::vector<SourceCharRange> blanks;
            const auto first=previous->glyph->index,last=item.glyph->index;
            if(last>first&&last-first<=64&&last<raw.size()){
                adjacent=true;
                for(auto i=first+1;i<last;++i){
                    if(space(raw[i].unicode))blanks.push_back({i,1});
                    else if(raw[i].unicode!=0xad&&!raw[i].unicode_continuation){adjacent=false;blanks.clear();break;}
                }
            }
            // Ink bounds contain side bearings: they are not glyph advances.
            // Prefer explicit/generated PDF spaces between neighboring records.
            if(!blanks.empty())append(block," ",std::move(blanks));
            else if(!adjacent&&gap>fragment.height*options.word_gap&&
                    (!(wide(c)&&wide(previous->glyph->unicode))||gap>fragment.height*0.8))append(block," ",{});
        }
        const bool join=previous&&previous->glyph->unicode!=0xad&&same_painted_glyph(*previous->glyph,*item.glyph);
        append_cluster(block,normalized(c),{item.glyph->index,item.glyph->source_count},join);previous=&item;
    }
    PdfRect rect=fragment.items.front().glyph->bounds;
    for(const auto& item:fragment.items){const auto r=item.glyph->bounds;rect={std::min(rect.left,r.left),std::min(rect.bottom,r.bottom),std::max(rect.right,r.right),std::max(rect.top,r.top)};}
    block.source_rects.push_back(rect);
    block.lines.push_back({line_begin,block.reading_text_utf8.size(),rect});
}
Fragments recognize_tables(Fragments fragments,const std::vector<GlyphRecord>& raw,
                           PageIndex page,double h,const LayoutOptions& options){
    std::vector<Fragments> rows;
    for(auto& f:fragments){
        if(rows.empty()||rows.back().front().baseline!=f.baseline)rows.push_back({});
        rows.back().push_back(std::move(f));
    }
    auto numeric=[](const Fragment& cell){
        bool digit=false;
        for(const auto& item:cell.items){
            const auto c=item.glyph->unicode;
            if(c>='0'&&c<='9')digit=true;
            else if(c!='.'&&c!=','&&c!='-'&&c!='+'&&c!='%'&&!space(c))return false;
        }
        return digit;
    };
    Fragments out;
    for(std::size_t i=0;i<rows.size();){
        std::size_t end=i+1;
        const auto columns=rows[i].size();
        if(columns>=3){
            while(end<rows.size()&&rows[end].size()==columns&&
                  rows[end].front().baseline-rows[end-1].front().baseline<=h*2.5){
                bool aligned=true;
                for(std::size_t c=0;c<columns;++c)
                    if(std::abs(rows[end][c].box.left-rows[i][c].box.left)>h*0.75||rows[end][c].items.size()>32)aligned=false;
                if(!aligned)break;
                ++end;
            }
        }
        std::size_t numeric_cells=0;
        for(auto r=i;r<end;++r)for(const auto& cell:rows[r])if(numeric(cell))++numeric_cells;
        if(end-i>=3&&columns>=3&&numeric_cells>=2*(end-i-1)){
            auto table=std::make_shared<TextBlock>();table->page=page;table->kind=BlockKind::Table;table->layout_confidence=0.95F;
            Rect box=rows[i].front().box;
            for(auto r=i;r<end;++r){
                if(r>i)append(*table,"\n",{});
                for(std::size_t c=0;c<columns;++c){
                    if(c)append(*table,"\t",{});
                    append_fragment(*table,rows[r][c],options,raw,false,false);box=unite(box,rows[r][c].box);
                }
            }
            Fragment placeholder{{},box,rows[i].front().baseline,h,rows[i].front().orientation,table};
            out.push_back(std::move(placeholder));i=end;
        }else{
            for(auto& f:rows[i])out.push_back(std::move(f));++i;
        }
    }
    return out;
}
bool equation(const TextBlock& block){
    const auto& text=block.reading_text_utf8;
    if(block.lines.size()!=1||text.size()>120||text.find_first_of(".?!:")!=std::string::npos)return false;
    if(text.find('=')==std::string::npos&&text.find("∑")==std::string::npos&&text.find("∫")==std::string::npos)return false;
    unsigned word=0;
    for(unsigned char c:text){
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')){if(++word>3)return false;}
        else word=0;
    }
    return true;
}
std::string block_id(const TextBlock& block){
    std::uint64_t hash=14695981039346656037ULL;
    for(unsigned char c:block.reading_text_utf8){hash^=c;hash*=1099511628211ULL;}
    std::ostringstream out;out<<"layout1-p"<<block.page<<"-o"<<block.reading_order<<'-'<<std::hex<<hash;return out.str();
}
std::string folded(std::string_view text){std::string out(text);for(char& c:out)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');return out;}
}

PageLayout LayoutEngine::build(PageIndex page,std::vector<GlyphRecord> glyphs,Affine2D matrix,double width,double height) const{
    PageLayout result;result.page=page;result.glyphs=std::move(glyphs);result.pdf_to_layout=matrix;
    if(result.glyphs.empty())return result;
    // The pinned PDFium can expose UTF-16 surrogate code units as two character
    // records for one ToUnicode mapping, even on platforms with 32-bit wchar_t.
    // Keep both original PDF indices while emitting one valid UTF-8 scalar.
    for(std::size_t i=0;i<result.glyphs.size();++i){
        auto& g=result.glyphs[i];if(g.unicode_continuation)continue;
        if(g.unicode>=0xd800&&g.unicode<=0xdbff&&i+1<result.glyphs.size()){
            auto& next=result.glyphs[i+1];
            if(next.unicode>=0xdc00&&next.unicode<=0xdfff&&next.index==g.index+1&&next.font_id==g.font_id&&
               std::hypot(next.origin.x-g.origin.x,next.origin.y-g.origin.y)<0.01&&std::abs(next.angle_radians-g.angle_radians)<0.00001){
                g.unicode=0x10000+((g.unicode-0xd800)<<10)+(next.unicode-0xdc00);g.source_count=2;
                next.unicode_continuation=true;
                if(next.valid_bounds){if(!g.valid_bounds)g.bounds=next.bounds;else g.bounds={std::min(g.bounds.left,next.bounds.left),std::min(g.bounds.bottom,next.bounds.bottom),std::max(g.bounds.right,next.bounds.right),std::max(g.bounds.top,next.bounds.top)};g.valid_bounds=true;}
                continue;
            }
        }
        if(g.unicode>=0xd800&&g.unicode<=0xdfff){g.unicode=0xfffd;g.unicode_map_error=true;}
    }
    std::array<std::vector<Item>,4> directions;
    std::vector<double> heights;
    bool unicode_available=false,usable_visible_text=false,visible_character=false;
    const double unit=(std::hypot(matrix.a,matrix.b)+std::hypot(matrix.c,matrix.d))/2;
    for(const auto& g:result.glyphs){
        if(g.unicode_continuation)continue;
        if(!g.valid_bounds)continue;
        if(!std::isfinite(g.bounds.left)||!std::isfinite(g.bounds.right)||!std::isfinite(g.bounds.top)||!std::isfinite(g.bounds.bottom)||
           !std::isfinite(g.origin.x)||!std::isfinite(g.origin.y)||!std::isfinite(g.font_size)||!std::isfinite(g.angle_radians))continue;
        const auto p0=matrix.apply({g.bounds.left,g.bounds.bottom}),p1=matrix.apply({g.bounds.right,g.bounds.top});
        Rect visible{std::min(p0.x,p1.x),std::min(p0.y,p1.y),std::max(p0.x,p1.x),std::max(p0.y,p1.y)};
        if(visible.right<=0||visible.left>=width||visible.bottom<=0||visible.top>=height)continue;
        visible_character=true;
        if(g.unicode&&g.unicode!=0xfffd&&!g.unicode_map_error)unicode_available=true;
        if(space(g.unicode))continue;
        if(g.unicode&&g.unicode!=0xfffd&&g.unicode!=0xad&&!g.unicode_map_error)usable_visible_text=true;
        auto origin=matrix.apply(g.origin);
        const auto along=matrix.apply({g.origin.x+std::cos(g.angle_radians),g.origin.y+std::sin(g.angle_radians)});
        int direction=(static_cast<int>(std::round(std::atan2(along.y-origin.y,along.x-origin.x)/(std::acos(-1)/2)))+4)%4;
        auto oriented=[&](PdfPoint p){switch(direction){case 1:return PdfPoint{p.y,-p.x};case 2:return PdfPoint{-p.x,-p.y};case 3:return PdfPoint{-p.y,p.x};default:return p;}};
        const auto a=oriented({visible.left,visible.top}),b=oriented({visible.right,visible.bottom});
        Rect box{std::min(a.x,b.x),std::min(a.y,b.y),std::max(a.x,b.x),std::max(a.y,b.y)};
        const double font_height=std::max(box.bottom-box.top,std::abs(g.font_size*unit));
        double sort_x=box.left;
        if(combining(g.unicode)&&g.index>0&&g.index<result.glyphs.size()){
            const auto& previous=result.glyphs[g.index-1];
            if(previous.valid_bounds&&!space(previous.unicode)){
                const auto pa=oriented(matrix.apply({previous.bounds.left,previous.bounds.bottom}));
                const auto pb=oriented(matrix.apply({previous.bounds.right,previous.bounds.top}));
                sort_x=std::min(pa.x,pb.x);
            }
        }
        directions[direction].push_back({&g,box,oriented(origin).y,font_height,sort_x});heights.push_back(font_height);
    }
    const auto typical=median(heights);
    for(int direction=0;direction<4;++direction){
        auto& items=directions[direction];
        std::sort(items.begin(),items.end(),[](const auto& a,const auto& b){if(a.baseline!=b.baseline)return a.baseline<b.baseline;return a.box.left<b.box.left;});
        std::vector<std::vector<Item>> rows;
        for(const auto& item:items){
            if(rows.empty()||std::abs(item.baseline-rows.back().front().baseline)>
               options_.baseline_tolerance*std::max(item.height,rows.back().front().height)+0.5*std::abs(item.height-rows.back().front().height))rows.push_back({});
            rows.back().push_back(item);
        }
        Fragments fragments;
        for(auto& row:rows){
            std::sort(row.begin(),row.end(),[](const auto& a,const auto& b){if(a.sort_x!=b.sort_x)return a.sort_x<b.sort_x;return a.glyph->index<b.glyph->index;});
            std::vector<double> sizes;for(const auto& item:row)sizes.push_back(item.height);const double h=median(sizes);
            for(const auto& item:row){
                if(fragments.empty()||fragments.back().baseline!=row.front().baseline||item.box.left-fragments.back().box.right>h*options_.fragment_gap)
                    fragments.push_back({{},item.box,row.front().baseline,h,direction});
                auto& f=fragments.back();f.items.push_back(item);f.box=unite(f.box,item.box);
            }
        }
        fragments=recognize_tables(std::move(fragments),result.glyphs,page,typical,options_);
        std::vector<Fragments> leaves;regions(std::move(fragments),typical,options_,0,leaves);
        for(const auto& leaf:leaves){
            const Fragment* previous=nullptr;
            TextBlock* block=nullptr;
            for(const auto& f:leaf){
                if(f.table){
                    auto table=*f.table;table.reading_order=static_cast<std::uint32_t>(result.blocks.size());
                    result.blocks.push_back(std::move(table));previous=nullptr;block=nullptr;continue;
                }
                const double overlap=previous?std::min(previous->box.right,f.box.right)-std::max(previous->box.left,f.box.left):0;
                const double span=previous?std::min(previous->box.right-previous->box.left,f.box.right-f.box.left):1;
                const bool same=previous&&f.baseline>previous->baseline&&f.baseline-previous->baseline<=options_.paragraph_baseline_gap*std::max(previous->height,f.height)&&
                    f.height/previous->height>0.72&&f.height/previous->height<1.38&&overlap>span*0.5;
                if(!same){
                    TextBlock b;b.page=page;b.reading_order=static_cast<std::uint32_t>(result.blocks.size());
                    b.kind=f.height>typical*1.35?BlockKind::Heading:BlockKind::Paragraph;b.layout_confidence=direction==0?0.85F:0.6F;
                    result.blocks.push_back(std::move(b));block=&result.blocks.back();
                }
                const bool no_space=previous&&(previous->items.back().glyph->unicode==0xad||
                    (wide(previous->items.back().glyph->unicode)&&wide(f.items.front().glyph->unicode)));
                append_fragment(*block,f,options_,result.glyphs,same,no_space);previous=&f;
            }
        }
    }
    bool has_model_block=false;
    for(auto& block:result.blocks){
        const auto lower=folded(block.reading_text_utf8);
        if(block.kind!=BlockKind::Table){
            if(lower.starts_with("figure ")||lower.starts_with("fig. ")||lower.starts_with("table "))block.kind=BlockKind::Caption;
            else if(equation(block))block.kind=BlockKind::Equation;
        }
        if(block.kind!=BlockKind::Table&&block.kind!=BlockKind::Equation&&!block.translation_input_utf8.empty())has_model_block=true;
        block.block_id=block_id(block);
        if(!result.reading_text_utf8.empty())result.reading_text_utf8+="\n\n";
        const auto offset=result.reading_text_utf8.size();result.reading_text_utf8+=block.reading_text_utf8;
        for(auto m:block.reading_mapping){m.utf8_begin+=offset;m.utf8_end+=offset;result.reading_mapping.push_back(std::move(m));}
        for(auto line:block.lines){line.utf8_begin+=offset;line.utf8_end+=offset;result.lines.push_back(line);}
    }
    result.availability=!visible_character?PageLayout::Availability::NoTranslatableText:!unicode_available?PageLayout::Availability::UnicodeUnavailable:
        usable_visible_text&&has_model_block?PageLayout::Availability::TextAvailable:PageLayout::Availability::NoTranslatableText;
    return result;
}
std::vector<SourceCharRange> source_ranges(const PageLayout& layout,std::size_t begin,std::size_t end){
    std::vector<SourceCharRange> result;
    for(const auto& m:layout.reading_mapping)if(m.utf8_begin<end&&m.utf8_end>begin)
        for(auto range:m.source_ranges)result.push_back(range);
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    std::vector<SourceCharRange> merged;
    for(auto range:result){
        if(!merged.empty()&&static_cast<std::uint64_t>(merged.back().first)+merged.back().count>=range.first){
            auto& prior=merged.back();prior.count=static_cast<std::uint32_t>(std::max<std::uint64_t>(static_cast<std::uint64_t>(prior.first)+prior.count,static_cast<std::uint64_t>(range.first)+range.count)-prior.first);
        }else merged.push_back(range);
    }
    return merged;
}
std::optional<TextHit> find_text(const PageLayout& layout,std::string_view needle,std::size_t start,bool match_case){
    if(needle.empty())return {};
    const auto haystack=match_case?layout.reading_text_utf8:folded(layout.reading_text_utf8);
    const auto query=match_case?std::string(needle):folded(needle);
    const auto at=haystack.find(query,start);if(at==std::string::npos)return {};
    return TextHit{at,at+query.size(),source_ranges(layout,at,at+query.size())};
}
std::optional<TextHit> find_text_backward(const PageLayout& layout,std::string_view needle,std::size_t before,bool match_case){
    if(needle.empty())return {};
    const auto haystack=match_case?layout.reading_text_utf8:folded(layout.reading_text_utf8);
    const auto query=match_case?std::string(needle):folded(needle);
    before=std::min(before,haystack.size());if(before<query.size())return {};
    const auto at=haystack.rfind(query,before-query.size());if(at==std::string::npos)return {};
    return TextHit{at,at+query.size(),source_ranges(layout,at,at+query.size())};
}
std::size_t nearest_text_position(const PageLayout& layout,PdfPoint point){
    const auto p=layout.pdf_to_layout.apply(point);
    auto rect=[&](PdfRect box){
        const auto a=layout.pdf_to_layout.apply({box.left,box.bottom}),b=layout.pdf_to_layout.apply({box.right,box.top});
        return Rect{std::min(a.x,b.x),std::min(a.y,b.y),std::max(a.x,b.x),std::max(a.y,b.y)};
    };
    auto distance=[&](Rect box){
        const double dx=std::max({box.left-p.x,0.0,p.x-box.right}),dy=std::max({box.top-p.y,0.0,p.y-box.bottom});
        return dx*dx+dy*dy;
    };
    double best_line=std::numeric_limits<double>::infinity();
    std::size_t begin=0,end=layout.reading_text_utf8.size();
    for(const auto& line:layout.lines){
        const auto d=distance(rect(line.source_bounds));
        if(d<best_line){best_line=d;begin=line.utf8_begin;end=line.utf8_end;}
    }
    auto first=std::lower_bound(layout.reading_mapping.begin(),layout.reading_mapping.end(),begin,
        [](const TextMapping& mapping,std::size_t value){return mapping.utf8_end<=value;});
    double best=std::numeric_limits<double>::infinity();std::size_t result=begin;
    for(auto m=first;m!=layout.reading_mapping.end()&&m->utf8_begin<end;++m)for(const auto range:m->source_ranges){
        for(std::uint64_t i=range.first;i<static_cast<std::uint64_t>(range.first)+range.count&&i<layout.glyphs.size();++i){
            const auto& g=layout.glyphs[static_cast<std::size_t>(i)];if(!g.valid_bounds||space(g.unicode)||g.unicode_continuation)continue;
            const auto box=rect(g.bounds);const auto d=distance(box);
            if(d<best){
                best=d;
                const auto origin=layout.pdf_to_layout.apply(g.origin);
                const auto along=layout.pdf_to_layout.apply({g.origin.x+std::cos(g.angle_radians),g.origin.y+std::sin(g.angle_radians)});
                const double projection=(p.x-(box.left+box.right)/2)*(along.x-origin.x)+
                    (p.y-(box.top+box.bottom)/2)*(along.y-origin.y);
                result=projection>0?m->utf8_end:m->utf8_begin;
            }
        }
    }
    return result;
}
bool near_text(const PageLayout& layout,PdfPoint point){
    const auto p=layout.pdf_to_layout.apply(point);
    for(const auto& line:layout.lines){
        const auto a=layout.pdf_to_layout.apply({line.source_bounds.left,line.source_bounds.bottom});
        const auto b=layout.pdf_to_layout.apply({line.source_bounds.right,line.source_bounds.top});
        const double left=std::min(a.x,b.x),right=std::max(a.x,b.x),top=std::min(a.y,b.y),bottom=std::max(a.y,b.y);
        const double padding=std::min(std::max(1.0,bottom-top),std::max(1.0,right-left))*0.4;
        if(p.x>=left-padding&&p.x<=right+padding&&p.y>=top-padding&&p.y<=bottom+padding)return true;
    }
    return false;
}
}
