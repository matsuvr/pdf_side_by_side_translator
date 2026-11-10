#include "app/controller.hpp"
#include "pdf/pdf_service.hpp"
#include "platform/font.hpp"
#include "ui/pdf_canvas.hpp"
#include "ui/wx_path.hpp"
#include "product.hpp"
#include <wx/wx.h>
#include <wx/combobox.h>
#include <wx/display.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/splitter.h>
#include <wx/stdpaths.h>
#include <wx/textdlg.h>
#include <wx/wrapsizer.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>

namespace opt {
namespace {
wxDEFINE_EVENT(PDF_WAKE,wxThreadEvent);
enum class Command { Open, Close, Previous, Next, GoPage, FitWidth, FitPage, Zoom, Left, Right, Equal, Copy, SelectAll, Help, About, Quit };
enum Id { Open=wxID_HIGHEST+400,Close,Previous,Next,GoPage,FitWidth,FitPage,Zoom,Left,Right,Equal,Help,About };
class PdfDropTarget final : public wxFileDropTarget {
    std::function<void(const wxString&)> open_;
public:
    explicit PdfDropTarget(std::function<void(const wxString&)> callback):open_(std::move(callback)){}
    bool OnDropFiles(wxCoord,wxCoord,const wxArrayString& names) override {
        if(names.empty())return false;open_(names[0]);return true;
    }
};
// Worker delivery has one pending wake event and a bounded raster payload budget.
class PdfInbox {
    std::mutex mutex_;
    std::deque<std::shared_ptr<const PdfEvent>> queue_;
    std::size_t bytes_=0;
    bool wake_=false;
    static std::size_t size(const std::shared_ptr<const PdfEvent>& e){return e->pixels?e->pixels->bgra.size():0;}
public:
    bool push(std::shared_ptr<const PdfEvent> e){
        std::lock_guard lock(mutex_);
        for(auto i=queue_.begin();i!=queue_.end();){
            if((*i)->document!=e->document||
               ((*i)->kind==e->kind&&(*i)->page==e->page&&(!e->pixels||(*i)->key==e->key))){
                bytes_-=size(*i);i=queue_.erase(i);
            }else ++i;
        }
        constexpr auto budget=32U*1024*1024;
        while(!queue_.empty()&&(bytes_+size(e)>budget||queue_.size()>=64)){
            auto victim=std::find_if(queue_.begin(),queue_.end(),[](const auto& p){return bool(p->pixels);});
            if(victim==queue_.end())victim=queue_.begin();
            bytes_-=size(*victim);queue_.erase(victim);
        }
        bytes_+=size(e);queue_.push_back(std::move(e));
        const bool notify=!wake_;wake_=true;return notify;
    }
    auto take(){
        std::lock_guard lock(mutex_);auto result=std::move(queue_);queue_.clear();bytes_=0;wake_=false;return result;
    }
};
}
struct ApplicationController::Impl {
    ApplicationController& owner;
    wxFrame* frame=nullptr;
    PdfCanvas* canvas=nullptr;
    wxTextCtrl* translation=nullptr;
    wxTextCtrl* page_input=nullptr;
    wxComboBox* zoom_input=nullptr;
    wxStaticText *page_count_label=nullptr,*left_heading=nullptr,*right_heading=nullptr;
    wxSplitterWindow* split=nullptr;
    wxWindow* toolbar=nullptr;
    wxButton* open_button=nullptr;
    wxTimer close_timer;
    PdfInbox inbox;
    std::atomic<DocumentEpoch> wanted_epoch{0};
    std::unique_ptr<PdfService> pdf;
    DocumentState document_state=DocumentState::Empty;
    DocumentEpoch epoch=0;
    PageIndex page=0;
    std::uint32_t page_count=0;
    std::filesystem::path path;
    std::map<PageIndex,PdfPoint> anchors;
    bool closing=false;

    explicit Impl(ApplicationController& receiver):owner(receiver),close_timer(&receiver){
        owner.Bind(PDF_WAKE,[this](wxThreadEvent&){for(auto& e:inbox.take())if(!closing)receive(*e);});
        owner.Bind(wxEVT_TIMER,[this](wxTimerEvent&){
            if(closing&&pdf->done()){
                close_timer.Stop();pdf->join();
                auto* window=frame;frame=nullptr;window->Destroy();
            }
        },close_timer.GetId());
        pdf=std::make_unique<PdfService>([this](auto event){
            if(event->document!=wanted_epoch.load())return;
            if(inbox.push(std::move(event)))wxQueueEvent(&owner,new wxThreadEvent(PDF_WAKE));
        });
    }
    ~Impl(){pdf->stop();pdf->join();}
    bool can(Command command) const {
        if(closing)return false;
        switch(command){
        case Command::Close:return document_state!=DocumentState::Empty;
        case Command::Previous:return document_state==DocumentState::Ready&&page>0;
        case Command::Next:return document_state==DocumentState::Ready&&page+1<page_count;
        case Command::GoPage:case Command::FitWidth:case Command::FitPage:case Command::Zoom:
        case Command::Left:case Command::Right:return document_state==DocumentState::Ready;
        default:return true;
        }
    }
    void route(int id,Command command){
        frame->Bind(wxEVT_MENU,[this,command](wxCommandEvent&){execute(command);},id);
        frame->Bind(wxEVT_BUTTON,[this,command](wxCommandEvent&){execute(command);},id);
        frame->Bind(wxEVT_UPDATE_UI,[this,command](wxUpdateUIEvent& e){e.Enable(can(command));},id);
    }
    void headings(){
        left_heading->SetLabel(page_count?wxString::Format("原文 — %uページ",page+1):"原文");
        right_heading->SetLabel(page_count?wxString::Format("訳文 — %uページ",page+1):"訳文");
        page_input->ChangeValue(page_count?wxString::Format("%u",page+1):wxString{});
        page_count_label->SetLabel(wxString::Format("/ %u",page_count));
        frame->SetStatusText(page_count?wxString::Format("%u / %uページ",page+1,page_count):"PDFを開いてください。",0);
        frame->Layout();
    }
    void open_path(const wxString& name){
        if(closing)return;
        path=native_path(name);document_state=DocumentState::Opening;page=0;page_count=0;
        anchors.clear();wanted_epoch=++epoch;
        canvas->set_page(epoch,0);
        translation->ChangeValue("PDFを読み込んでいます…");
        frame->SetTitle(display_path(path.filename())+" — "+wxString::FromUTF8(product::name));
        headings();frame->SetStatusText("PDFを読み込んでいます…",0);
        pdf->open(epoch,path);
    }
    void go(PageIndex target){
        if(target>=page_count||target==page)return;
        if(auto a=canvas->reading_anchor())anchors[page]=*a;
        page=target;canvas->set_page(epoch,page);translation->ChangeValue("翻訳モデルを設定すると、このページの訳文を表示します。");
        headings();pdf->page_info(epoch,page);
    }
    void receive(const PdfEvent& event){
        if(event.document!=epoch)return;
        switch(event.kind){
        case PdfEvent::Kind::Opened:
            document_state=DocumentState::Ready;page_count=event.page_count;page=0;
            canvas->set_page(epoch,0,event.info);headings();
            translation->ChangeValue("翻訳モデルを設定すると、このページの訳文を表示します。");
            break;
        case PdfEvent::Kind::PageInfo:
            if(event.page==page){canvas->set_info(event.info);if(auto i=anchors.find(page);i!=anchors.end())canvas->restore_anchor(i->second);}
            break;
        case PdfEvent::Kind::ViewReady:case PdfEvent::Kind::TileReady:
            canvas->accept(event);break;
        case PdfEvent::Kind::PasswordRequired:{
            wxPasswordEntryDialog dialog(frame,"PDFのパスワードを入力してください。","パスワード付きPDF");
            if(dialog.ShowModal()==wxID_OK)pdf->open(epoch,path,dialog.GetValue().ToStdString(wxConvUTF8));
            else execute(Command::Close);
            break;
        }
        case PdfEvent::Kind::Failed:
            if(event.code=="PdfOpen"){document_state=DocumentState::Failed;translation->ChangeValue(wxString::FromUTF8(event.message));}
            if(event.code=="PdfOpen"||event.page==page)frame->SetStatusText(wxString::FromUTF8(event.message),0);
            break;
        default:break;
        }
    }
    void execute(Command command){
        if(!can(command))return;
        switch(command){
        case Command::Open:{
            wxFileDialog dialog(frame,"PDFを開く",{}, {},"PDFファイル (*.pdf)|*.pdf|すべてのファイル|*",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
            if(dialog.ShowModal()==wxID_OK)open_path(dialog.GetPath());break;
        }
        case Command::Close:
            wanted_epoch=++epoch;pdf->close(epoch);document_state=DocumentState::Empty;page_count=0;page=0;
            anchors.clear();canvas->clear_document();translation->Clear();headings();frame->SetTitle(wxString::FromUTF8(product::name));break;
        case Command::Previous:go(page-1);break;case Command::Next:go(page+1);break;
        case Command::GoPage:{
            long value=0;auto text=page_input->GetValue();
            if(text.ToLong(&value)&&value>=1&&static_cast<unsigned long>(value)<=page_count)go(static_cast<PageIndex>(value-1));
            else frame->SetStatusText("有効なページ番号を入力してください。",0);
            page_input->ChangeValue(wxString::Format("%u",page+1));break;
        }
        case Command::FitWidth:canvas->set_zoom(ZoomMode::FitWidth);zoom_input->ChangeValue("幅に合わせる");break;
        case Command::FitPage:canvas->set_zoom(ZoomMode::FitPage);zoom_input->ChangeValue("ページ全体");break;
        case Command::Zoom:{
            auto value=zoom_input->GetValue();
            if(value=="幅に合わせる"){execute(Command::FitWidth);break;}
            if(value=="ページ全体"){execute(Command::FitPage);break;}
            value.Trim(true).Trim(false);if(value.EndsWith("%"))value.RemoveLast();
            double percent=0;
            if(value.ToDouble(&percent)&&std::isfinite(percent)&&percent>0){canvas->set_zoom(ZoomMode::Percent,percent);zoom_input->ChangeValue(wxString::Format("%.1f%%",canvas->zoom_percent()));}
            else {frame->SetStatusText("正の倍率（%）を入力してください。",0);zoom_input->ChangeValue(wxString::Format("%.1f%%",canvas->zoom_percent()));}
            break;
        }
        case Command::Left:canvas->rotate(-1);break;case Command::Right:canvas->rotate(1);break;
        case Command::Equal:split->SetSashPosition(split->GetClientSize().x/2);break;
        case Command::Copy:case Command::SelectAll:
            if(auto* native=wxDynamicCast(wxWindow::FindFocus(),wxTextCtrl)){if(command==Command::Copy)native->Copy();else native->SelectAll();}
            break;
        case Command::Help:
            wxMessageBox("PDFを開くか、ウィンドウへドロップしてください。\nページ番号はEnterで確定します。\n\nホイール: ページ内をスクロール\nShift＋ホイール: 横スクロール\nPrimary＋ホイール: 拡大・縮小\nSpace＋ドラッグ／中ボタン: 移動\nF6: ペイン間のフォーカス移動\n\nPDFの原本は変更しません。","操作方法",wxOK|wxICON_INFORMATION,frame);break;
        case Command::About:
            wxMessageBox(wxString::FromUTF8(product::name)+" "+wxString::FromUTF8(product::version)+"\nオフラインPDF翻訳アプリ\nMIT License","このアプリについて",wxOK,frame);break;
        case Command::Quit:frame->Close();break;
        }
    }
    void start(const wxString& initial){
        std::string error;
        const bool font_ok=register_private_font(native_path(wxStandardPaths::Get().GetResourcesDir()),error);
        frame=new wxFrame(nullptr,wxID_ANY,wxString::FromUTF8(product::name));
        auto area=wxDisplay(0U).GetClientArea();auto size=frame->FromDIP(wxSize(1280,860));
        frame->SetSize(wxSize(std::min(size.x,area.width),std::min(size.y,area.height)));frame->Centre();
        frame->SetMinSize(frame->FromDIP(wxSize(640,400)));
        auto file_menu=new wxMenu;file_menu->Append(Open,"PDFを開く…\tCtrl+O");file_menu->Append(Close,"文書を閉じる\tCtrl+W");file_menu->AppendSeparator();file_menu->Append(wxID_EXIT,"終了\tCtrl+Q");
        auto edit_menu=new wxMenu;edit_menu->Append(wxID_COPY,"コピー\tCtrl+C");edit_menu->Append(wxID_SELECTALL,"全選択\tCtrl+A");
        auto view_menu=new wxMenu;
        view_menu->Append(FitWidth,"幅に合わせる");view_menu->Append(FitPage,"ページ全体");
        view_menu->Append(Left,"左回転");view_menu->Append(Right,"右回転");view_menu->Append(Equal,"左右を均等にする");
#ifdef __WXMAC__
        view_menu->Append(Previous,"前のページ\tCtrl+[");view_menu->Append(Next,"次のページ\tCtrl+]");
#else
        view_menu->Append(Previous,"前のページ\tAlt+PageUp");view_menu->Append(Next,"次のページ\tAlt+PageDown");
#endif
        auto help_menu=new wxMenu;help_menu->Append(Help,"操作方法");help_menu->Append(About,"このアプリについて");
        auto menu=new wxMenuBar;menu->Append(file_menu,"ファイル");menu->Append(edit_menu,"編集");menu->Append(view_menu,"表示");menu->Append(help_menu,"ヘルプ");frame->SetMenuBar(menu);
        route(Open,Command::Open);route(Close,Command::Close);route(Previous,Command::Previous);route(Next,Command::Next);
        route(GoPage,Command::GoPage);route(FitWidth,Command::FitWidth);route(FitPage,Command::FitPage);route(Zoom,Command::Zoom);
        route(Left,Command::Left);route(Right,Command::Right);route(Equal,Command::Equal);route(Help,Command::Help);route(About,Command::About);
        route(wxID_COPY,Command::Copy);route(wxID_SELECTALL,Command::SelectAll);route(wxID_EXIT,Command::Quit);
        auto root=new wxBoxSizer(wxVERTICAL);
        toolbar=new wxPanel(frame);auto tools=new wxWrapSizer(wxHORIZONTAL);
        const int gap=frame->FromDIP(4);
        auto button=[&](int id,const wxString& label){auto* b=new wxButton(toolbar,id,label);tools->Add(b,0,wxALL,gap);return b;};
        open_button=button(Open,"PDFを開く");button(Previous,"前へ");button(Next,"次へ");
        page_input=new wxTextCtrl(toolbar,GoPage,{},wxDefaultPosition,frame->FromDIP(wxSize(68,-1)),wxTE_PROCESS_ENTER);page_input->SetName("ページ番号");
        tools->Add(page_input,0,wxALL|wxALIGN_CENTER_VERTICAL,gap);
        page_count_label=new wxStaticText(toolbar,wxID_ANY,"/ 0");tools->Add(page_count_label,0,wxALL|wxALIGN_CENTER_VERTICAL,gap);
        wxArrayString options;for(const auto* label:{"幅に合わせる","ページ全体","50%","75%","100%","125%","150%","200%","400%"})options.Add(wxString::FromUTF8(label));
        zoom_input=new wxComboBox(toolbar,Zoom,"幅に合わせる",wxDefaultPosition,frame->FromDIP(wxSize(160,-1)),options,wxTE_PROCESS_ENTER);zoom_input->SetName("PDFの倍率");tools->Add(zoom_input,0,wxALL|wxALIGN_CENTER_VERTICAL,gap);
        toolbar->SetSizer(tools);root->Add(toolbar,0,wxEXPAND);
        split=new wxSplitterWindow(frame,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxSP_LIVE_UPDATE|wxSP_3D);
        auto left=new wxPanel(split);auto right=new wxPanel(split);auto ls=new wxBoxSizer(wxVERTICAL);auto rs=new wxBoxSizer(wxVERTICAL);
        left_heading=new wxStaticText(left,wxID_ANY,"原文");right_heading=new wxStaticText(right,wxID_ANY,"訳文");
        ls->Add(left_heading,0,wxALL,gap);rs->Add(right_heading,0,wxALL,gap);
        canvas=new PdfCanvas(left,[this](ViewRequest request){pdf->viewport(std::move(request));});ls->Add(canvas,1,wxEXPAND);
        long text_style=wxTE_MULTILINE|wxTE_READONLY;
#ifdef __WXMSW__
        text_style|=wxTE_RICH2;
#endif
        translation=new wxTextCtrl(right,wxID_ANY,{},wxDefaultPosition,wxDefaultSize,text_style);translation->SetName("現在ページの日本語訳");
        auto font=translation->GetFont();if(font_ok)font.SetFaceName("Noto Sans CJK JP");font.SetPointSize(14);translation->SetFont(font);rs->Add(translation,1,wxEXPAND);
        left->SetSizer(ls);right->SetSizer(rs);split->SplitVertically(left,right,frame->GetClientSize().x/2);split->SetSashGravity(0.5);split->SetMinimumPaneSize(frame->FromDIP(120));root->Add(split,1,wxEXPAND);frame->SetSizer(root);
        frame->CreateStatusBar(3);int widths[]{-2,-1,-1};frame->SetStatusWidths(3,widths);frame->SetStatusText("モデル未設定",1);
        if(!font_ok)frame->SetStatusText("同梱フォントを登録できませんでした。",2);
        frame->SetDropTarget(new PdfDropTarget([this](const wxString& name){open_path(name);}));
        canvas->SetDropTarget(new PdfDropTarget([this](const wxString& name){open_path(name);}));
        page_input->Bind(wxEVT_TEXT_ENTER,[this](wxCommandEvent&){execute(Command::GoPage);});
        page_input->Bind(wxEVT_KILL_FOCUS,[this](wxFocusEvent& e){if(page_count)execute(Command::GoPage);e.Skip();});
        zoom_input->Bind(wxEVT_TEXT_ENTER,[this](wxCommandEvent&){execute(Command::Zoom);});zoom_input->Bind(wxEVT_COMBOBOX,[this](wxCommandEvent&){execute(Command::Zoom);});
        frame->Bind(wxEVT_CHAR_HOOK,[this](wxKeyEvent& e){
            if(e.GetKeyCode()==WXK_F6){
                wxWindow* targets[]{canvas,translation,open_button};int index=0;
                for(int i=0;i<3;++i)if(targets[i]==wxWindow::FindFocus())index=i;
                targets[(index+(e.ShiftDown()?2:1))%3]->SetFocus();
            }else e.Skip();
        });
        frame->Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent& e){
            if(!closing){closing=true;pdf->stop();frame->Disable();frame->SetStatusText("終了処理中…",0);close_timer.Start(50);}
            if(e.CanVeto())e.Veto();
        });
        headings();frame->Show();frame->Layout();split->SetSashPosition(split->GetClientSize().x/2);
        if(!initial.empty())open_path(initial);
    }
};
ApplicationController::ApplicationController():impl_(std::make_unique<Impl>(*this)){}
ApplicationController::~ApplicationController()=default;
void ApplicationController::start(const wxString& initial){impl_->start(initial);}
}
