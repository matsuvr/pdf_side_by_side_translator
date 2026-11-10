#include "ui/pdf_canvas.hpp"
#include <wx/dcbuffer.h>
#include <wx/settings.h>
#include <wx/graphics.h>
#include <wx/clipbrd.h>
#include <wx/menu.h>
#include <algorithm>
#include <cmath>
#include <climits>

namespace opt {
namespace {constexpr int render_flags=1;constexpr std::size_t gui_cache_budget=64*1024*1024;}
PdfCanvas::PdfCanvas(wxWindow* parent,ViewSink sink):wxScrolledWindow(parent,wxID_ANY),sink_(std::move(sink)){
    SetName(wxS("PDF原文"));SetBackgroundStyle(wxBG_STYLE_PAINT);SetScrollRate(1,1);
    Bind(wxEVT_PAINT,&PdfCanvas::paint,this);
    Bind(wxEVT_SIZE,[this](wxSizeEvent& e){calculate();e.Skip();});
    for(const auto& type:{wxEVT_SCROLLWIN_TOP,wxEVT_SCROLLWIN_BOTTOM,wxEVT_SCROLLWIN_LINEUP,
        wxEVT_SCROLLWIN_LINEDOWN,wxEVT_SCROLLWIN_PAGEUP,wxEVT_SCROLLWIN_PAGEDOWN,
        wxEVT_SCROLLWIN_THUMBTRACK,wxEVT_SCROLLWIN_THUMBRELEASE})
        Bind(type,[this](wxScrollWinEvent& e){schedule();e.Skip();});
    Bind(wxEVT_TIMER,[this](wxTimerEvent&){request_visible();},timer_.GetId());
    Bind(wxEVT_MOUSEWHEEL,[this](wxMouseEvent& e){
        if((e.ControlDown()||e.CmdDown())&&info_){
            zoom_mode_=ZoomMode::Percent;percent_*=e.GetWheelRotation()>0?1.15:1/1.15;
            calculate(e.GetPosition());
        }else if(e.ShiftDown()){
            int x,y;GetViewStart(&x,&y);Scroll(std::max(0,x-e.GetWheelRotation()/e.GetWheelDelta()*FromDIP(60)),y);schedule();
        }else {schedule();e.Skip();}
    });
    auto begin_pan=[this](wxMouseEvent& e){
        SetFocus();if(e.MiddleDown()||space_){
            selecting_=false;
            panning_=true;drag_start_=e.GetPosition();GetViewStart(&scroll_start_.x,&scroll_start_.y);
            if(!HasCapture())CaptureMouse();SetCursor(wxCursor(wxCURSOR_HAND));
        }else if(layout_&&transform_){
            const auto pixel=page_pixel(e.GetPosition());
            const auto point=transform_->page_px_to_pdf.apply(pixel);
            if(pixel.x>=0&&pixel.y>=0&&pixel.x<width_&&pixel.y<height_&&near_text(*layout_,point)){
                const auto at=nearest_text_position(*layout_,point);selection_=std::pair{at,at};selecting_=true;
                if(!HasCapture())CaptureMouse();SetCursor(wxCursor(wxCURSOR_IBEAM));
            }else selection_.reset();
            Refresh();
        }else e.Skip();
    };
    Bind(wxEVT_LEFT_DOWN,begin_pan);Bind(wxEVT_MIDDLE_DOWN,begin_pan);
    auto end_pan=[this](wxMouseEvent& e){
        if(panning_||selecting_){panning_=false;selecting_=false;if(HasCapture())ReleaseMouse();SetCursor(wxNullCursor);}else e.Skip();
    };
    Bind(wxEVT_LEFT_UP,end_pan);Bind(wxEVT_MIDDLE_UP,end_pan);
    Bind(wxEVT_MOTION,[this](wxMouseEvent& e){
        if(panning_){const auto delta=e.GetPosition()-drag_start_;Scroll(std::max(0,scroll_start_.x-delta.x),std::max(0,scroll_start_.y-delta.y));schedule();}
        else if(selecting_&&selection_&&layout_&&transform_){
            selection_->second=nearest_text_position(*layout_,transform_->page_px_to_pdf.apply(page_pixel(e.GetPosition())));Refresh();
        }else e.Skip();
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST,[this](wxMouseCaptureLostEvent&){panning_=false;selecting_=false;SetCursor(wxNullCursor);});
    Bind(wxEVT_CONTEXT_MENU,[this](wxContextMenuEvent&){
        SetFocus();wxMenu menu;menu.Append(wxID_COPY,wxS("コピー"));menu.Enable(wxID_COPY,has_selection());
        menu.Append(wxID_SELECTALL,wxS("全選択"));menu.Enable(wxID_SELECTALL,layout_&&!layout_->reading_text_utf8.empty());PopupMenu(&menu);
    });
    Bind(wxEVT_KEY_DOWN,[this](wxKeyEvent& e){
        int x,y;GetViewStart(&x,&y);const int step=FromDIP(32);
        switch(e.GetKeyCode()){
        case WXK_SPACE:space_=true;break;
        case WXK_ESCAPE:clear_selection();return;
        case WXK_LEFT:x-=step;break;case WXK_RIGHT:x+=step;break;
        case WXK_UP:y-=step;break;case WXK_DOWN:y+=step;break;
        case WXK_PAGEUP:y-=GetClientSize().y*9/10;break;case WXK_PAGEDOWN:y+=GetClientSize().y*9/10;break;
        case WXK_HOME:y=0;break;case WXK_END:y=GetVirtualSize().y;break;
        default:e.Skip();return;
        }
        Scroll(std::max(0,x),std::max(0,y));schedule();
    });
    Bind(wxEVT_KEY_UP,[this](wxKeyEvent& e){if(e.GetKeyCode()==WXK_SPACE)space_=false;else e.Skip();});
    Bind(wxEVT_KILL_FOCUS,[this](wxFocusEvent& e){space_=false;e.Skip();});
    Bind(wxEVT_DPI_CHANGED,[this](wxDPIChangedEvent& e){calculate();e.Skip();});
}
void PdfCanvas::clear_document(){timer_.Stop();document_=0;info_.reset();transform_.reset();pending_anchor_.reset();layout_.reset();selection_.reset();search_range_.reset();cache_.clear();cache_bytes_=0;width_=height_=0;last_request_.reset();SetVirtualSize(1,1);Refresh();}
void PdfCanvas::set_page(DocumentEpoch document,PageIndex page,std::optional<PdfPageInfo> info){
    if(document_!=document){cache_.clear();cache_bytes_=0;}
    document_=document;page_=page;info_=info;transform_.reset();pending_anchor_.reset();last_request_.reset();
    layout_.reset();selection_.reset();search_range_.reset();
    Scroll(0,0);width_=height_=0;calculate();Refresh();
}
void PdfCanvas::set_info(const PdfPageInfo& info){info_=info;calculate();}
void PdfCanvas::set_layout(std::shared_ptr<const PageLayout> layout){
    if(layout_==layout)return;layout_=std::move(layout);selection_.reset();Refresh();
}
bool PdfCanvas::has_selection() const noexcept{return selection_&&selection_->first!=selection_->second&&bool(layout_);}
void PdfCanvas::clear_selection(){selection_.reset();Refresh();}
void PdfCanvas::select_all(){
    if(layout_){selection_=std::pair<std::size_t,std::size_t>{0,layout_->reading_text_utf8.size()};Refresh();}
}
bool PdfCanvas::copy_selection() const{
    if(!has_selection())return false;
    const auto [a,b]=*selection_;const auto first=std::min(a,b),last=std::max(a,b);
    const auto text=std::string_view(layout_->reading_text_utf8).substr(first,last-first);
    if(!wxTheClipboard->Open())return false;
    wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text.data(),text.size())));wxTheClipboard->Close();return true;
}
void PdfCanvas::show_search_hit(std::optional<TextHit> hit){
    search_range_.reset();
    if(hit&&layout_){
        search_range_=std::pair{hit->begin,hit->end};
        if(!hit->source_ranges.empty()){
            const auto index=hit->source_ranges.front().first;
            if(index<layout_->glyphs.size()){
                const auto& rect=layout_->glyphs[index].bounds;
                pending_anchor_=Anchor{{(rect.left+rect.right)/2,(rect.bottom+rect.top)/2},{GetClientSize().x/2,GetClientSize().y/3}};schedule();
            }
        }
    }
    Refresh();
}
PdfPoint PdfCanvas::page_pixel(wxPoint screen) const{
    int x,y;GetViewStart(&x,&y);
    return {(static_cast<double>(x)+screen.x-origin_.x)*scale_,(static_cast<double>(y)+screen.y-origin_.y)*scale_};
}
std::optional<PdfPoint> PdfCanvas::reading_anchor() const{
    if(!transform_)return {};
    return transform_->page_px_to_pdf.apply(page_pixel({0,0}));
}
void PdfCanvas::restore_anchor(PdfPoint point){pending_anchor_=Anchor{point,{0,0}};schedule();}
void PdfCanvas::set_zoom(ZoomMode mode,double percent){
    if(!std::isfinite(percent)||percent<=0)return;
    zoom_mode_=mode;percent_=percent;calculate();
}
void PdfCanvas::rotate(int delta){
    if(transform_)pending_anchor_=Anchor{transform_->page_px_to_pdf.apply(page_pixel({client_.x/2,client_.y/2})),{GetClientSize().x/2,GetClientSize().y/2}};
    rotation_=(rotation_+delta%4+4)%4;calculate();
}
void PdfCanvas::calculate(std::optional<wxPoint> pointer){
    const auto client=GetClientSize();
    if(!info_){client_=client;SetVirtualSize(1,1);Refresh();return;}
    if(transform_&&!pending_anchor_){
        const auto oldpoint=pointer.value_or(wxPoint(client_.x/2,client_.y/2));
        const auto newpoint=pointer.value_or(wxPoint(client.x/2,client.y/2));
        pending_anchor_=Anchor{transform_->page_px_to_pdf.apply(page_pixel(oldpoint)),newpoint};
    }
    client_=client;scale_=std::max(1.0,GetContentScaleFactor());
    const int margin=FromDIP(8);
    const long double pw=rotation_%2?info_->height_points:info_->width_points;
    const long double ph=rotation_%2?info_->width_points:info_->height_points;
    const long double dpi=static_cast<long double>(FromDIP(1000))/1000;
    const long double base=96.0L/72*dpi*scale_;
    long double multiplier=base*percent_/100;
    if(zoom_mode_!=ZoomMode::Percent){
        multiplier=std::max(1,client.x-margin*2)*scale_/pw;
        if(zoom_mode_==ZoomMode::FitPage)multiplier=std::min(multiplier,std::max(1,client.y-margin*2)*scale_/ph);
    }
    const long double max_size=INT_MAX-std::max(1024,margin*4);
    multiplier=std::min(multiplier,std::min(max_size/pw,max_size/ph));
    width_=std::max(1,static_cast<int>(std::round(pw*multiplier)));
    height_=std::max(1,static_cast<int>(std::round(ph*multiplier)));
    percent_=static_cast<double>(multiplier/base*100);
    const int local_width=static_cast<int>(std::ceil(width_/scale_));
    const int local_height=static_cast<int>(std::ceil(height_/scale_));
    origin_={std::max(margin,(client.x-local_width)/2),margin};
    SetVirtualSize(std::max(client.x,local_width+2*margin),std::max(client.y,local_height+2*margin));
    transform_.reset();last_request_.reset();schedule();Refresh();
}
bool PdfCanvas::matches(const TileKey& k) const{return k.document==document_&&k.page==page_&&k.full_width==width_&&k.full_height==height_&&k.rotation==rotation_;}
void PdfCanvas::schedule(){if(document_&&info_)timer_.StartOnce(16);}
void PdfCanvas::request_visible(){
    if(!document_||!info_||width_<=0||height_<=0)return;
    int sx,sy;GetViewStart(&sx,&sy);
    const auto client=GetClientSize();
    const auto left=std::clamp((static_cast<double>(sx)-origin_.x)*scale_,0.0,static_cast<double>(width_-1));
    const auto top=std::clamp((static_cast<double>(sy)-origin_.y)*scale_,0.0,static_cast<double>(height_-1));
    const auto right=std::clamp((static_cast<double>(sx)+client.x-origin_.x)*scale_,0.0,static_cast<double>(width_));
    const auto bottom=std::clamp((static_cast<double>(sy)+client.y-origin_.y)*scale_,0.0,static_cast<double>(height_));
    ViewRequest request{document_,page_,width_,height_,rotation_,{}};
    for(std::int64_t y=static_cast<int>(top)/512*512;y<bottom&&request.tiles.size()<64;y+=512)
        for(std::int64_t x=static_cast<int>(left)/512*512;x<right&&request.tiles.size()<64;x+=512){
            TileKey key{document_,page_,width_,height_,rotation_,render_flags,static_cast<int>(x),static_cast<int>(y)};
            if(!cache_.contains(key))request.tiles.emplace_back(key.x,key.y);
        }
    if(last_request_&&last_request_->document==request.document&&last_request_->page==request.page&&
       last_request_->width==request.width&&last_request_->height==request.height&&
       last_request_->rotation==request.rotation&&last_request_->tiles==request.tiles&&!pending_anchor_)return;
    last_request_=request;sink_(std::move(request));
}
void PdfCanvas::accept(const PdfEvent& e){
    if(e.document!=document_)return;
    if(e.kind==PdfEvent::Kind::ViewReady&&matches(e.key)){
        transform_=e.transform;
        if(pending_anchor_){
            const auto p=e.transform.pdf_to_page_px.apply(pending_anchor_->pdf);
            const auto screen=pending_anchor_->screen;
            const double x=origin_.x+p.x/scale_-screen.x,y=origin_.y+p.y/scale_-screen.y;
            Scroll(static_cast<int>(std::clamp(x,0.0,static_cast<double>(std::max(0,GetVirtualSize().x-GetClientSize().x)))),
                   static_cast<int>(std::clamp(y,0.0,static_cast<double>(std::max(0,GetVirtualSize().y-GetClientSize().y)))));
            pending_anchor_.reset();schedule();
        }
    }
    if(e.kind!=PdfEvent::Kind::TileReady||!e.pixels)return;
    const auto& r=*e.pixels;
    wxImage image(r.width,r.height);
    auto* rgb=image.GetData();
    for(int y=0;y<r.height;++y)for(int x=0;x<r.width;++x){
        const auto i=static_cast<std::size_t>(y)*r.stride+x*4,j=(static_cast<std::size_t>(y)*r.width+x)*3;
        rgb[j]=r.bgra[i+2];rgb[j+1]=r.bgra[i+1];rgb[j+2]=r.bgra[i];
    }
    wxBitmap bitmap(image);bitmap.SetScaleFactor(scale_);
    const auto bytes=static_cast<std::size_t>(r.width)*r.height*4;
    if(auto old=cache_.find(e.key);old!=cache_.end())cache_bytes_-=old->second.bytes;
    cache_.insert_or_assign(e.key,CachedTile{std::move(bitmap),bytes,++tick_});cache_bytes_+=bytes;
    while(cache_bytes_>gui_cache_budget){
        auto victim=std::min_element(cache_.begin(),cache_.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
        cache_bytes_-=victim->second.bytes;cache_.erase(victim);
    }
    if(matches(e.key)){Refresh();schedule();}
}
void PdfCanvas::paint(wxPaintEvent&){
    wxAutoBufferedPaintDC dc(this);PrepareDC(dc);
    dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_APPWORKSPACE)));dc.Clear();
    if(!info_){
        dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        dc.DrawText(document_?wxS("ページを読み込んでいます…"):wxS("PDFを開くか、ここへドロップしてください。"),FromDIP(16),FromDIP(16));return;
    }
    dc.SetPen(*wxTRANSPARENT_PEN);dc.SetBrush(*wxWHITE_BRUSH);
    dc.DrawRectangle(origin_.x,origin_.y,static_cast<int>(std::ceil(width_/scale_)),static_cast<int>(std::ceil(height_/scale_)));
    for(auto& [key,tile]:cache_)if(matches(key)){
        tile.used=++tick_;
        tile.bitmap.SetScaleFactor(scale_);
        dc.DrawBitmap(tile.bitmap,origin_.x+static_cast<int>(std::round(key.x/scale_)),origin_.y+static_cast<int>(std::round(key.y/scale_)),false);
    }
    if(layout_&&transform_){
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if(gc){
            gc->Clip(origin_.x,origin_.y,width_/scale_,height_/scale_);gc->SetPen(*wxTRANSPARENT_PEN);
            auto highlight=[&](const auto& range,wxColour colour){
                if(!range)return;
                gc->SetBrush(wxBrush(colour));const auto [a,b]=*range;
                for(const auto source:source_ranges(*layout_,std::min(a,b),std::max(a,b)))
                    for(std::uint64_t i=source.first;i<static_cast<std::uint64_t>(source.first)+source.count&&i<layout_->glyphs.size();++i){
                        const auto& g=layout_->glyphs[static_cast<std::size_t>(i)];if(!g.valid_bounds)continue;
                        const auto p=transform_->pdf_to_page_px.apply({g.bounds.left,g.bounds.bottom});
                        const auto q=transform_->pdf_to_page_px.apply({g.bounds.right,g.bounds.top});
                        gc->DrawRectangle(origin_.x+std::min(p.x,q.x)/scale_,origin_.y+std::min(p.y,q.y)/scale_,
                            std::max(1.0,std::abs(p.x-q.x)/scale_),std::max(1.0,std::abs(p.y-q.y)/scale_));
                    }
            };
            highlight(search_range_,wxColour(255,190,20,90));highlight(selection_,wxColour(60,130,245,85));
        }
    }
}
}
