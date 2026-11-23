#include "ui/image_reading_bar.hpp"
#include "image_reading_assets.hpp"
#include <wx/wx.h>
#ifdef __WXMSW__
#include <wx/graphics.h>
#include <wx/renderer.h>
#include <windows.h>
#include <memory>
#endif
#include <algorithm>

namespace opt {
namespace {
const wxString toggle_label=wxS("画像から読み取り (OCR)");
wxString display(std::string_view label){return wxString::FromUTF8(label.data(),label.size());}
wxColour subdued_text(wxWindow* window){
    const auto fg=wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    return fg.ChangeLightness(window->GetBackgroundColour().GetLuminance()>0.5?140:75);
}
#ifdef __WXMSW__
void draw_focus(wxWindow* control,wxDC& dc){
    if(control->HasFocus()&&!(SendMessageW(control->GetHandle(),WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS))
        wxRendererNative::Get().DrawFocusRect(control,dc,control->GetClientRect());
}
#endif
}

ImageReadingBar::ImageReadingBar(wxWindow* parent,std::function<void(bool)> toggle,
                                 std::function<void(const ImageLanguage*)> correct)
    :wxPanel(parent),correct_(std::move(correct)){
    long word_style=wxBU_EXACTFIT;
#ifdef __WXMSW__
    word_style|=wxBORDER_NONE;
#endif
    word_=new wxButton(this,wxID_ANY,{},wxDefaultPosition,wxDefaultSize,word_style);
    suffix_=new wxStaticText(this,wxID_ANY,wxS("として読み取り"));
    toggle_=new wxCheckBox(this,wxID_ANY,toggle_label);
    word_->SetCursor(wxCursor(wxCURSOR_HAND));
    word_->SetToolTip(wxS("別の言語で読み直す"));
    word_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){choose_language();});
    toggle_->Bind(wxEVT_CHECKBOX,[toggle=std::move(toggle)](wxCommandEvent& e){toggle(e.IsChecked());});
#ifdef __WXMSW__
    // Keep the native button/checkbox input and accessibility semantics.
    word_->Bind(wxEVT_PAINT,[this](wxPaintEvent&){
        wxPaintDC dc(word_);
        dc.SetBackground(wxBrush(GetBackgroundColour()));dc.Clear();
        dc.SetFont(word_->GetFont());dc.SetTextForeground(subdued_text(this));
        const auto size=word_->GetTextExtent(word_->GetLabel());
        const int x=FromDIP(2),y=(word_->GetClientSize().y-size.y)/2;
        dc.DrawText(word_->GetLabel(),x,y);
        dc.SetPen(wxPen(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT),1,wxPENSTYLE_DOT));
        dc.DrawLine(x,y+size.y,x+size.x,y+size.y);
        draw_focus(word_,dc);
    });
    toggle_->Bind(wxEVT_PAINT,[this](wxPaintEvent&){
        wxPaintDC dc(toggle_);
        dc.SetBackground(wxBrush(GetBackgroundColour()));dc.Clear();
        const bool on=toggle_->GetValue();
        const auto track=wxSystemSettings::GetColour(on?wxSYS_COLOUR_HIGHLIGHT:wxSYS_COLOUR_GRAYTEXT);
        const int pad=FromDIP(2),width=FromDIP(34),height=FromDIP(18);
        const int top=(toggle_->GetClientSize().y-height)/2;
        if(std::unique_ptr<wxGraphicsContext> gc{wxGraphicsContext::Create(dc)}){
            gc->SetPen(*wxTRANSPARENT_PEN);gc->SetBrush(wxBrush(track));
            gc->DrawRoundedRectangle(pad,top,width,height,height/2.);
            gc->SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT)));
            gc->DrawEllipse(pad+(on?width-height:0)+pad,top+pad,height-2*pad,height-2*pad);
        }
        dc.SetFont(toggle_->GetFont());dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        wxRect label=toggle_->GetClientRect();label.x+=width+FromDIP(9);label.width-=label.x;
        dc.DrawLabel(toggle_text_,label,wxALIGN_CENTER_VERTICAL|wxALIGN_LEFT);
        draw_focus(toggle_,dc);
    });
#endif
    Bind(wxEVT_SIZE,[this](wxSizeEvent& event){arrange();event.Skip();});
    update(false,nullptr,{},false);
}

void ImageReadingBar::update(bool enabled,const ImageLanguage* manual,const ImageReadingResult& result,bool failed){
    ++revision_;
    manual_=manual;current_=result.language;alternatives_=result.alternatives;
    const bool read=enabled&&!failed&&current_;
    const bool select=enabled&&failed&&(result.script_reader||current_||manual_);
    if(read){
        const auto label=display(current_->label);
        word_->SetLabel(label);word_->SetName(label+wxS("、別の言語で読み直す"));
        suffix_->SetForegroundColour(subdued_text(this));
    }else if(select){
        word_->SetLabel(wxS("言語を選択"));word_->SetName(word_->GetLabel());
    }
    if(!read&&!select&&word_->HasFocus())toggle_->SetFocus();
    word_->Show(read||select);suffix_->Show(read);toggle_->SetValue(enabled);
    arrange();word_->Refresh();toggle_->Refresh();
}

void ImageReadingBar::arrange(){
    const int width=GetClientSize().x,pad=FromDIP(6),row_height=FromDIP(30);
    const int room=std::max(1,width-2*pad);
    int switch_width=toggle_->GetTextExtent(toggle_label).x+FromDIP(46);
    toggle_text_=switch_width>room?wxS("画像から\n読み取り (OCR)"):toggle_label;
    if(switch_width>room&&toggle_->GetTextExtent(wxS("読み取り (OCR)")).x+FromDIP(46)>room)
        toggle_text_=wxS("画像から\n読み取り\n(OCR)");
#ifndef __WXMSW__
    if(toggle_->GetLabel()!=toggle_text_)toggle_->SetLabel(toggle_text_);
#endif
    const int lines=1+static_cast<int>(toggle_text_.Freq('\n'));
    const int switch_height=std::max(row_height,lines*toggle_->GetCharHeight()+2*pad);
    switch_width=std::min(room,switch_width);
    int top=0;
    if(word_->IsShown()){
        int word_width=word_->GetTextExtent(word_->GetLabel()).x+FromDIP(4);
#ifndef __WXMSW__
        word_width=std::max(word_width,word_->GetBestSize().x);
#endif
        const auto suffix=suffix_->IsShown()?suffix_->GetBestSize():wxSize(0,0);
        const int word_row=std::max(row_height,suffix.y+2*pad);
        word_->SetSize(pad,0,word_width,word_row);
        const bool wrap_suffix=word_width+suffix.x>room;
        suffix_->SetSize(wrap_suffix?pad:pad+word_width,(wrap_suffix?word_row:0)+(word_row-suffix.y)/2,suffix.x,suffix.y);
        if(word_width+suffix.x+pad+switch_width>room)top=word_row*(wrap_suffix?2:1);
    }
    toggle_->SetSize(width-pad-switch_width,top,switch_width,switch_height);
    const int height=top+switch_height;
    if(GetMinHeight()!=height){
        SetMinSize(wxSize(1,height));
        if(GetParent()->GetSizer())GetParent()->Layout();
    }
}

void ImageReadingBar::choose_language(){
    if(!word_->IsShown()||!toggle_->GetValue())return;
    const auto revision=revision_;
    wxMenu menu;
    menu.Append(wxID_ANY,wxS("別の言語で読み直す"))->Enable(false);
    const auto add=[](wxMenu& list,const ImageLanguage* lang){
        list.Append(wxID_HIGHEST+1+static_cast<int>(lang-image_languages),display(lang->label));
    };
    for(const auto* lang:alternatives_)if(lang)add(menu,lang);
    menu.AppendSeparator();
    auto* more=new wxMenu;
    for(const auto& lang:image_languages){
        if(&lang!=current_ && &lang!=manual_ && std::find(alternatives_.begin(),alternatives_.end(),&lang)==alternatives_.end())
            add(*more,&lang);
    }
    menu.AppendSubMenu(more,wxS("その他の言語"));
    constexpr int automatic=wxID_HIGHEST+1+std::size(image_languages);
    if(manual_){menu.AppendSeparator();menu.Append(automatic,wxS("自動判別に戻す"));}
    const int choice=word_->GetPopupMenuSelectionFromUser(menu,0,word_->GetClientSize().y);
    if(choice==wxID_NONE||revision!=revision_)return;
    if(choice==automatic)correct_(nullptr);
    else if(choice>wxID_HIGHEST&&choice<automatic)correct_(&image_languages[choice-wxID_HIGHEST-1]);
}
}
