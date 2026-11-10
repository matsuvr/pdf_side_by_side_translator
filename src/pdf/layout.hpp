#pragma once
#include "core/types.hpp"
#include <string_view>
#include <optional>

namespace opt {
struct LayoutOptions {
    double baseline_tolerance=0.35;
    double fragment_gap=1.5;
    double word_gap=0.35;
    double horizontal_band_gap=2.5;
    double minimum_gutter=1.2;
    double paragraph_baseline_gap=1.8;
    unsigned maximum_depth=32;
};
class LayoutEngine {
public:
    explicit LayoutEngine(LayoutOptions options={}):options_(options){}
    PageLayout build(PageIndex page,std::vector<GlyphRecord> glyphs,
                     Affine2D pdf_to_layout,double width,double height) const;
private:
    LayoutOptions options_;
};
struct TextHit {
    std::size_t begin=0,end=0;
    std::vector<SourceCharRange> source_ranges;
};
std::vector<SourceCharRange> source_ranges(const PageLayout& layout,std::size_t begin,std::size_t end);
std::optional<TextHit> find_text(const PageLayout& layout,std::string_view needle,
                               std::size_t start,bool match_case);
std::optional<TextHit> find_text_backward(const PageLayout& layout,std::string_view needle,
                                        std::size_t before,bool match_case);
std::size_t nearest_text_position(const PageLayout& layout,PdfPoint point);
bool near_text(const PageLayout& layout,PdfPoint point);
}
