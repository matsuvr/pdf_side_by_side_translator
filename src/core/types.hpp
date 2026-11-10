#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace opt {
using DocumentEpoch = std::uint64_t;
using ModelEpoch = std::uint64_t;
using SourceRevision = std::uint64_t;
using SettingsRevision = std::uint64_t;
using JobId = std::uint64_t;
using PlanGeneration = std::uint64_t;
using PageIndex = std::uint32_t;
using PdfCharIndex = std::uint32_t;
struct PdfPoint { double x=0, y=0;bool operator==(const PdfPoint&)const=default; };
struct PdfRect { double left=0, bottom=0, right=0, top=0; };
struct Affine2D {
    double a=1,b=0,c=0,d=1,tx=0,ty=0;
    PdfPoint apply(PdfPoint p) const noexcept {return {a*p.x+c*p.y+tx,b*p.x+d*p.y+ty};}
    Affine2D inverse() const;
};
struct WorkStamp {
    DocumentEpoch document=0;
    PageIndex page=0;
    ModelEpoch model=0;
    SourceRevision source=0;
    SettingsRevision settings=0;
    std::string page_attempt;
    PlanGeneration plan=0;
    JobId job=0;
    bool operator==(const WorkStamp&) const = default;
};
enum class DocumentState { Empty, Opening, Ready, Failed };
enum class ModelState { Unconfigured, Selected, Loading, Ready, Failed };
enum class PageState { NotStarted, WaitingForText, Queued, Running, Complete, Partial, Failed, NoText };
struct SourceCharRange { PdfCharIndex first=0;std::uint32_t count=0; };
struct TextMapping {
    std::size_t utf8_begin=0,utf8_end=0;
    std::vector<SourceCharRange> source_ranges;
};
struct GlyphRecord {
    PdfCharIndex index=0;
    char32_t unicode=0;
    PdfRect bounds;
    PdfPoint origin;
    double font_size=0,angle_radians=0;
    std::uint32_t font_id=0;
    bool generated=false,valid_bounds=false,unicode_map_error=false;
    std::uint32_t source_count=1;
    bool unicode_continuation=false;
};
enum class BlockKind { Heading, Paragraph, Caption, Equation, Table, Margin, Unknown, Edited };
struct TextLine {
    std::size_t utf8_begin=0,utf8_end=0;
    PdfRect source_bounds;
};
struct TextBlock {
    std::string block_id;
    PageIndex page=0;
    BlockKind kind=BlockKind::Unknown;
    std::uint32_t reading_order=0;
    std::string reading_text_utf8,translation_input_utf8;
    std::vector<TextMapping> reading_mapping,translation_mapping;
    std::vector<PdfRect> source_rects;
    std::vector<TextLine> lines;
    float layout_confidence=0;
};
struct PageLayout {
    enum class Availability { TextAvailable, NoTextLayer, UnicodeUnavailable, NoTranslatableText };
    PageIndex page=0;
    static constexpr std::uint32_t current_version=2;
    std::uint32_t layout_version=current_version;
    std::vector<GlyphRecord> glyphs;
    std::vector<TextBlock> blocks;
    std::string reading_text_utf8;
    std::vector<TextMapping> reading_mapping;
    std::vector<TextLine> lines;
    Affine2D pdf_to_layout;
    Availability availability=Availability::NoTextLayer;
};
std::string encode_utf8(char32_t scalar);
}
