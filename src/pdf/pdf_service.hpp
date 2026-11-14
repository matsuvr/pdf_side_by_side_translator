#pragma once
#include "core/types.hpp"
#include "pdf/layout.hpp"
#include "platform/file_signature.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace opt {
class FileSnapshot;
// FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE. Kept here so UI tile identities
// use exactly the worker's render flags without importing PDFium into the UI.
inline constexpr int pdf_render_flags=0x001|0x200;
inline constexpr int max_page_pixels=262144; // Per axis, including zoom and device scale.
struct TileKey {
    DocumentEpoch document=0;
    PageIndex page=0;
    int full_width=0,full_height=0,rotation=0,flags=0;
    int x=0,y=0;
    auto operator<=>(const TileKey&) const = default;
};
struct RenderTransform {
    int full_width_px=0,full_height_px=0,user_rotation_quarters=0;
    Affine2D pdf_to_page_px,page_px_to_pdf;
};
struct RasterBuffer {
    int width=0,height=0,stride=0;
    std::vector<unsigned char> bgra;
};
struct PdfPageInfo {
    double width_points=0,height_points=0;
    Affine2D pdf_to_layout;
    double layout_width=1024,layout_height=1024;
};
struct ViewRequest {
    DocumentEpoch document=0;
    PageIndex page=0;
    int width=0,height=0,rotation=0;
    // Physical pixel origins, always aligned to the 512-pixel tile grid.
    std::vector<std::pair<int,int>> tiles;
};
struct PdfSearchRequest {
    DocumentEpoch document=0;
    PageIndex page=0;
    std::uint64_t generation=0;
    std::string query;
    std::size_t start=0;
    bool match_case=false,backward=false;
    std::shared_ptr<const PageLayout> cached_layout;
};
struct PdfEvent {
    enum class Kind { Opened, Closed, PageInfo, ViewReady, TileReady, Extracted, ExtractionDeferred, SearchResult, PasswordRequired, Failed };
    Kind kind=Kind::Failed;
    DocumentEpoch document=0;
    PageIndex page=0;
    std::uint32_t page_count=0;
    std::shared_ptr<const FileSnapshot> file_snapshot;
    FileSignature file_signature;
    PdfPageInfo info;
    // Opened is still emitted when only the first page cannot be described, so
    // the remaining pages of a damaged document stay viewable.
    bool has_page_info=true;
    TileKey key;
    RenderTransform transform;
    std::shared_ptr<const RasterBuffer> pixels;
    std::shared_ptr<const PageLayout> layout;
    std::uint64_t query_generation=0;
    std::optional<TextHit> hit;
    std::string code,message,detail;
};
struct PdfServiceStats {
    std::uint64_t page_cache_peak=0,extraction_queue_peak=0,tile_bytes_peak=0,render_slices=0,render_yields=0,text_slices=0,text_yields=0,max_render_slice_us=0;
};
class PdfService {
public:
    using Delivery=std::function<void(std::shared_ptr<const PdfEvent>)>;
    explicit PdfService(Delivery delivery,std::filesystem::path font_resource_directory={},LayoutOptions layout_options={});
    ~PdfService();
    PdfService(const PdfService&)=delete;
    PdfService& operator=(const PdfService&)=delete;
    void open(DocumentEpoch epoch,const std::filesystem::path& path,std::string_view password={});
    void close(DocumentEpoch epoch);
    void page_info(DocumentEpoch epoch,PageIndex page);
    void viewport(ViewRequest request);
    bool extract(DocumentEpoch epoch,PageIndex page);
    void search(PdfSearchRequest request);
    void cancel_search(DocumentEpoch epoch,std::uint64_t generation);
    void stop() noexcept;
    bool done() const noexcept;
    void join();
    PdfServiceStats stats()const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
