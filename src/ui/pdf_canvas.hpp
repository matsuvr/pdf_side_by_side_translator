#pragma once
#include "pdf/pdf_service.hpp"
#include "pdf/layout.hpp"
#include <wx/scrolwin.h>
#include <wx/timer.h>
#include <map>
#include <optional>

namespace opt {
enum class ZoomMode { FitWidth, FitPage, Percent };
class PdfCanvas final : public wxScrolledWindow {
public:
    using ViewSink=std::function<void(ViewRequest)>;
    PdfCanvas(wxWindow* parent,ViewSink sink);
    void set_page(DocumentEpoch document,PageIndex page,std::optional<PdfPageInfo> info={});
    void set_info(const PdfPageInfo& info);
    void set_zoom(ZoomMode mode,double percentage=100);
    void rotate(int quarter_delta);
    void accept(const PdfEvent& event);
    void clear_document();
    void set_layout(std::shared_ptr<const PageLayout> layout);
    bool copy_selection() const;
    void select_all();
    void clear_selection();
    bool has_selection() const noexcept;
    void show_search_hit(std::optional<TextHit> hit);
    std::optional<PdfPoint> reading_anchor() const;
    void restore_anchor(PdfPoint point);
    ZoomMode zoom_mode() const noexcept{return zoom_mode_;}
    double zoom_percent() const noexcept{return percent_;}
    int rotation() const noexcept{return rotation_;}
private:
    struct CachedTile {wxBitmap bitmap;std::size_t bytes;std::uint64_t used;};
    struct Anchor {PdfPoint pdf;wxPoint screen;};
    ViewSink sink_;
    wxTimer timer_{this};
    DocumentEpoch document_=0;
    PageIndex page_=0;
    std::optional<PdfPageInfo> info_;
    std::optional<RenderTransform> transform_;
    std::optional<Anchor> pending_anchor_;
    ZoomMode zoom_mode_=ZoomMode::FitWidth;
    double percent_=100,scale_=1;
    int rotation_=0,width_=0,height_=0;
    wxPoint origin_,drag_start_,scroll_start_;
    wxSize client_;
    bool panning_=false,space_=false;
    bool selecting_=false;
    std::shared_ptr<const PageLayout> layout_;
    std::optional<std::pair<std::size_t,std::size_t>> selection_,search_range_;
    std::map<TileKey,CachedTile> cache_;
    std::size_t cache_bytes_=0;
    std::uint64_t tick_=0;
    std::optional<ViewRequest> last_request_;
    void calculate(std::optional<wxPoint> pointer={});
    void request_visible();
    void schedule();
    void paint(wxPaintEvent& event);
    bool matches(const TileKey& key) const;
    PdfPoint page_pixel(wxPoint point) const;
};
}
