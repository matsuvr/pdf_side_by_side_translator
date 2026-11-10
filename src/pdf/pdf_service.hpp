#pragma once
#include "core/types.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace opt {
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
};
struct ViewRequest {
    DocumentEpoch document=0;
    PageIndex page=0;
    int width=0,height=0,rotation=0;
    // Physical pixel origins, always aligned to the 512-pixel tile grid.
    std::vector<std::pair<int,int>> tiles;
};
struct PdfEvent {
    enum class Kind { Opened, Closed, PageInfo, ViewReady, TileReady, Extracted, PasswordRequired, Failed };
    Kind kind=Kind::Failed;
    DocumentEpoch document=0;
    PageIndex page=0;
    std::uint32_t page_count=0;
    PdfPageInfo info;
    TileKey key;
    RenderTransform transform;
    std::shared_ptr<const RasterBuffer> pixels;
    std::shared_ptr<const std::vector<GlyphRecord>> glyphs;
    std::string code,message,detail;
};
class PdfService {
public:
    using Delivery=std::function<void(std::shared_ptr<const PdfEvent>)>;
    explicit PdfService(Delivery delivery);
    ~PdfService();
    PdfService(const PdfService&)=delete;
    PdfService& operator=(const PdfService&)=delete;
    void open(DocumentEpoch epoch,const std::filesystem::path& path,std::string password={});
    void close(DocumentEpoch epoch);
    void page_info(DocumentEpoch epoch,PageIndex page);
    void viewport(ViewRequest request);
    bool extract(DocumentEpoch epoch,PageIndex page);
    void stop() noexcept;
    bool done() const noexcept;
    void join();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
