// Run in an interactive desktop session; this exercises the real native widget.
#include "platform/font.hpp"
#include "platform/text_view.hpp"
#include <wx/wx.h>
#include <wx/stdpaths.h>
#include <cmath>
#include <filesystem>
#include <iostream>
#ifdef __APPLE__
#import <AppKit/AppKit.h>
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <richedit.h>
#endif
namespace {
double scroll_y(wxTextCtrl& control){
#ifdef __APPLE__
    return [[(NSScrollView*)control.GetHandle() contentView] bounds].origin.y;
#else
    POINT point{};SendMessageW((HWND)control.GetHandle(),EM_GETSCROLLPOS,0,(LPARAM)&point);return point.y;
#endif
}
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
#ifdef _WIN32
void check_streamed_wrap(wxTextCtrl& control){
    const auto handle=static_cast<HWND>(control.GetHandle());
    require(IsWindowVisible(handle),"wrap repaint test needs a visible text control");
    SendMessageW(handle,EM_EMPTYUNDOBUFFER,0,0);
    std::string value="確定した訳文の先頭。\n\n";
    opt::update_readonly_text(control,{},value,true);
    UpdateWindow(handle);
    const auto initial_lines=SendMessageW(handle,EM_GETLINECOUNT,0,0);
    const auto second_line=SendMessageW(handle,EM_LINEINDEX,1,0);
    POINTL second_line_origin{};
    SendMessageW(handle,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&second_line_origin),second_line);
    require(second_line_origin.y>0,"first line is on screen");
    for(int i=0;i<20;++i){
        auto next=value+"割引率は、";
        opt::update_readonly_text(control,value,next,false);
        RECT dirty{};
        require(!GetUpdateRect(handle,&dirty,FALSE)||dirty.top>=second_line_origin.y,
                "streaming a wrapped tail invalidated the unchanged first line");
        UpdateWindow(handle);
        value=std::move(next);
    }
    require(SendMessageW(handle,EM_GETLINECOUNT,0,0)>initial_lines,"Japanese stream crossed a soft wrap");
    require(control.GetValue()==wxString::FromUTF8(value),"soft wrapping preserves the streamed text");
    for(int i=0;i<60;++i){
        auto next=value+"次の行へ折り返しても末尾を追従する。";
        opt::update_readonly_text(control,value,next,false);
        value=std::move(next);
    }
    SCROLLINFO scroll{sizeof(SCROLLINFO),SIF_ALL};
    GetScrollInfo(handle,SB_VERT,&scroll);
    require(scroll.nPos>0 && scroll.nPos+static_cast<int>(scroll.nPage)>=scroll.nMax-2,
            "stream follows the bottom as wrapped lines outgrow the viewport");
    require(!SendMessageW(handle,EM_CANUNDO,0,0),"generated text has no user undo history");
}
#endif
class TextTest final:public wxApp {
    wxFrame* frame_=nullptr;wxTextCtrl *text_=nullptr,*focus_=nullptr;int result_=1;
    wxTimer timer_{this};
    void run(){
        try{
#ifdef _WIN32
            check_streamed_wrap(*text_);
#endif
            std::string value;for(int i=0;i<800;++i)value+="読書位置 "+std::to_string(i)+" — 結合文字 é・絵文字 👩‍💻・日本語の長い訳文。\n";
            opt::update_readonly_text(*text_,{},value,true);
            const auto intended=value.find("読書位置 300");text_->SetSelection(120,150);
            opt::restore_text_logical_anchor(*text_,intended);focus_->SetFocus();
            const auto y=scroll_y(*text_);require(y>200,"long text scrolled away from top");
            const auto anchor=opt::text_logical_anchor(*text_);
            auto* focus_before=wxWindow::FindFocus();require(focus_before==focus_,"native focus established before stream");
            for(int i=0;i<80;++i){auto next=value+"追記 "+std::to_string(i)+" 👩‍💻\r\n";opt::update_readonly_text(*text_,value,next,false);value=std::move(next);}
            long begin=0,end=0;text_->GetSelection(&begin,&end);
            require(begin==120&&end==150,"native selection survives repeated streaming appends");
            require(wxWindow::FindFocus()==focus_before,"stream does not steal native focus");
            require(std::abs(scroll_y(*text_)-y)<=1,"native pixel viewport survives streaming");
            auto font=text_->GetFont();font.SetPointSize(21);text_->SetFont(font);text_->SetSelection(begin,end);opt::restore_text_logical_anchor(*text_,anchor);
            auto restored=opt::text_logical_anchor(*text_);
            require(restored<=anchor&&anchor-restored<250,"font resize restores same logical line");
            const auto before_resize=restored;
            frame_->SetClientSize(610,410);frame_->Layout();opt::restore_text_logical_anchor(*text_,before_resize);
            restored=opt::text_logical_anchor(*text_);
            require(restored<=before_resize&&before_resize-restored<250,"window resize restores logical reading line");
            text_->GetSelection(&begin,&end);require(begin==120&&end==150,"font and window size preserve selection");
            // Replacing a partial result edits the changed suffix once.
            const auto replacement=value.substr(0,value.rfind("追記"))+"置換した末尾。";
            opt::update_readonly_text(*text_,value,replacement,false);
            require(text_->GetValue().Contains(wxS("置換した末尾。")),"partial-result replacement reaches native widget");
            std::cout<<"native stream: selection, focus, pixel viewport, font/resize logical anchors and suffix replacement passed\n";result_=0;
        }catch(const std::exception& e){std::cerr<<e.what()<<'\n';}
        frame_->Destroy();
    }
public:
    bool OnInit()override{
        std::string error;const auto resource_directory=wxStandardPaths::Get().GetResourcesDir();const auto resources=resource_directory.ToUTF8();
        if(!opt::register_private_font(std::filesystem::u8path(resources.data()),error)){
            std::cerr<<"Bundled font: "<<error<<" resources="<<resources.data()<<'\n';return false;
        }
        frame_=new wxFrame(nullptr,wxID_ANY,"Native text acceptance");auto* sizer=new wxBoxSizer(wxVERTICAL);
        focus_=new wxTextCtrl(frame_,wxID_ANY,"focus remains here");sizer->Add(focus_,0,wxEXPAND);
        long style=wxTE_MULTILINE|wxTE_READONLY;
#ifdef _WIN32
        style|=wxTE_RICH2;
#endif
        text_=new wxTextCtrl(frame_,wxID_ANY,{},wxDefaultPosition,wxDefaultSize,style);text_->SetMaxLength(0);
#ifdef _WIN32
        text_->SetDoubleBuffered(true);
#endif
        auto font=text_->GetFont();font.SetFaceName("Noto Sans CJK JP");font.SetPointSize(14);text_->SetFont(font);
        sizer->Add(text_,1,wxEXPAND);frame_->SetSizer(sizer);frame_->SetClientSize(820,600);frame_->Show();frame_->Layout();
        frame_->Raise();
#ifdef __APPLE__
        [NSApp activateIgnoringOtherApps:YES];
#else
        SetForegroundWindow((HWND)frame_->GetHandle());
#endif
        focus_->SetFocus();Bind(wxEVT_TIMER,[this](wxTimerEvent&){run();},timer_.GetId());timer_.StartOnce(250);return true;
    }
    int OnRun()override{wxApp::OnRun();return result_;}
};
}
wxIMPLEMENT_APP(TextTest);
