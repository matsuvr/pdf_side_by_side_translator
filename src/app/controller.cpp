#include "app/controller.hpp"
#include "app/bundled_files.hpp"
#include "app/resources.hpp"
#include "pdf/pdf_service.hpp"
#include "pdf/delivery_inbox.hpp"
#include "platform/font.hpp"
#include "platform/text_view.hpp"
#include "platform/input_state.hpp"
#include "inference/inference_service.hpp"
#include "inference/delivery_inbox.hpp"
#include "translation/page_attempt.hpp"
#include "platform/digest.hpp"
#include "storage/io_service.hpp"
#include "storage/diagnostic_log.hpp"
#include "ui/pdf_canvas.hpp"
#include "ui/wx_path.hpp"
#include "ui/shortcut_policy.hpp"
#include "product.hpp"
#include "build_info.hpp"
#include <wx/wx.h>
#include <wx/combobox.h>
#include <wx/display.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/splitter.h>
#include <wx/stdpaths.h>
#include <wx/textdlg.h>
#include <wx/secretstore.h>
#include <wx/numdlg.h>
#include <wx/wrapsizer.h>
#include <wx/clipbrd.h>
#include <wx/checkbox.h>
#include <wx/weakref.h>
#include <wx/mstream.h>
#include <wx/file.h>
#include <wx/filesys.h>
#include <wx/hyperlink.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <limits>
#include <chrono>
#include <cstdlib>
#include <system_error>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
#include <utility>
#include <sstream>

namespace opt {
namespace {
wxDEFINE_EVENT(PDF_WAKE,wxThreadEvent);
wxDEFINE_EVENT(IO_WAKE,wxThreadEvent);
enum class Command { Open, Close, Previous, Next, GoPage, FocusPage, FitWidth, FitPage, Zoom, Left, Right, Equal, TextSize, Copy, SelectAll, CopySource, Find, FindNext, FindPrevious, CloseSearch, Automatic, Plain, Easy, ModelSettings, Help, Diagnostics, Licenses, About, Quit };
enum Id { Open=wxID_HIGHEST+400,Close,Previous,Next,GoPage,FocusPage,FitWidth,FitPage,Zoom,Left,Right,Equal,TextSize,Help,Diagnostics,Licenses,About,CopySource,Find,FindNext,FindPrevious,CloseSearch,Automatic,Plain,Easy,ModelSettings };
std::filesystem::path application_directory(){
#ifdef __WXMAC__
    constexpr auto layout=ExecutableLayout::MacAppBundle;
#else
    constexpr auto layout=ExecutableLayout::Standalone;
#endif
    return distribution_directory(native_path(wxStandardPaths::Get().GetExecutablePath()),layout);
}
wxString model_license_path(){
    return display_path(application_directory()/"PLaMo-Community-ja.txt");
}
[[noreturn]] void terminate_stalled_process() noexcept {
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
#endif
    std::_Exit(EXIT_FAILURE);
}
void arm_exit_deadline() {
    // Bound stalled worker and GUI teardown.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    try {
        std::thread([deadline] {
            std::this_thread::sleep_until(deadline);
            terminate_stalled_process();
        }).detach();
    } catch (const std::system_error&) {
        terminate_stalled_process();
    }
}
struct ModalScope {unsigned& depth;explicit ModalScope(unsigned& value):depth(value){++depth;}~ModalScope(){--depth;}};
bool copy_text(std::string_view text){
    if(!wxTheClipboard->Open())return false;
    wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text.data(),text.size())));wxTheClipboard->Close();return true;
}
class PdfDropTarget final : public wxFileDropTarget {
    std::function<void(const wxString&)> open_;
public:
    explicit PdfDropTarget(std::function<void(const wxString&)> callback):open_(std::move(callback)){}
    bool OnDropFiles(wxCoord,wxCoord,const wxArrayString& names) override {
        if(names.empty())return false;open_(names[0]);return true;
    }
};
class PdfPasswordDialog final : public wxPasswordEntryDialog {
public:
    using wxPasswordEntryDialog::wxPasswordEntryDialog;
    ~PdfPasswordDialog() override {wxSecretValue::WipeString(m_value);}
    void submit(PdfService& pdf,DocumentEpoch epoch,const std::filesystem::path& path){
        auto bytes=wxConvUTF8.cWC2MB(m_value.wc_str());
        struct WipeBuffer {
            wxCharBuffer& bytes;
            ~WipeBuffer(){if(bytes.data())wxSecretValue::Wipe(bytes.length(),bytes.data());}
        } wipe{bytes};
        pdf.open(epoch,path,bytes.data()?std::string_view(bytes.data(),bytes.length()):std::string_view{});
    }
};
class IoInbox {
    std::mutex mutex_;std::deque<std::shared_ptr<const IoEvent>> queue_;bool wake_=false;
public:
    bool push(std::shared_ptr<const IoEvent> e){
        std::lock_guard lock(mutex_);
        // Snapshot acknowledgements only update status; keep the latest one
        // so they do not displace pending read responses.
        if(e->kind==IoEvent::Kind::AttemptSaved)
            std::erase_if(queue_,[&](const auto& old){return old->kind==e->kind;});
        while(queue_.size()>=64)queue_.pop_front();
        queue_.push_back(std::move(e));const bool notify=!wake_;wake_=true;return notify;
    }
    auto take(){
        std::lock_guard lock(mutex_);auto result=std::move(queue_);queue_.clear();wake_=false;
        return result;
    }
};
}
struct ApplicationController::Impl {
    ApplicationController& owner;
    wxFrame* frame=nullptr;
    PdfCanvas* canvas=nullptr;
    wxTextCtrl* translation=nullptr;
    wxScrolledWindow* welcome=nullptr;
    wxHyperlinkCtrl* welcome_license=nullptr;
    wxTextCtrl* page_input=nullptr;
    wxComboBox* zoom_input=nullptr;
    wxStaticText *page_count_label=nullptr,*left_heading=nullptr,*right_heading=nullptr;
    wxSplitterWindow* split=nullptr;
    wxButton* open_button=nullptr;
    wxTimer search_timer;
    wxTimer translate_timer,stream_timer;
    wxTimer position_timer;
    wxWindow *search_bar=nullptr,*left_container=nullptr;
    wxTextCtrl* search_input=nullptr;
    wxCheckBox* search_case=nullptr;
    wxStaticText* search_label=nullptr;
    wxWeakRef<wxWindow> search_return_focus;
    unsigned modal_depth=0;
    struct SearchState {
        std::uint64_t generation=0;
        std::string query;
        bool active=false,backward=false,skipped_page=false;
        PageIndex cursor=0;
        std::size_t start=0;
        std::uint32_t remaining=0;
        std::optional<std::pair<PageIndex,TextHit>> hit;
    } search;
    PdfInbox inbox;
    InferenceInbox inference_inbox;
    IoInbox io_inbox;
    std::atomic<DocumentEpoch> wanted_epoch{0};
    std::atomic<ModelEpoch> wanted_model{0};
    std::unique_ptr<PdfService> pdf;
    std::unique_ptr<InferenceService> inference;
    std::unique_ptr<IoService> io;
    std::unique_ptr<NativeShortcutMonitor> native_shortcuts;
    std::string document_key,native_document_key,model_key;
    std::uint32_t model_capacity=8192;
    ModelProfile model_profile;
    InferenceRuntime inference_runtime;
    double model_load_seconds=0,last_chunk_seconds=0;
    std::deque<std::string> recent_diagnostics;
    CacheContext attempt_context;
    struct PendingAttempt {std::uint64_t request;WorkStamp stamp;CacheContext context;std::shared_ptr<const std::string> source;};
    std::optional<PendingAttempt> pending_attempt;
    std::uint64_t next_io_request=0;
    bool document_state_loaded=false,reading_touched=false,page_position_touched=false,restoring_reading=false,storage_warning=false;
    bool preferences_loaded=false;
    std::set<PageIndex> loaded_positions,requested_positions;
    std::vector<std::string> recent_paths;
    wxMenu* recent_menu=nullptr;
    std::optional<ReadingPosition> last_saved_position;
    std::optional<std::pair<PageIndex,std::size_t>> pending_text_anchor;
    int text_size=14;
    struct ResizeAnchor {DocumentEpoch document;PageIndex page;std::size_t offset;};
    std::optional<ResizeAnchor> resize_text_anchor;
    bool resize_restore_queued=false;
    ModelState model_state=ModelState::Unconfigured;
    ModelEpoch model_epoch=0;
    SettingsRevision settings_revision=1;
    std::filesystem::path model_path;
    Form form=Form::Plain;
    std::string backend,model_error,shown_translation;
    bool auto_translate=true,translation_dirty=false;
    std::unique_ptr<PageAttempt> attempt;
    std::optional<PageIndex> scheduled_page;
    DocumentState document_state=DocumentState::Empty;
    DocumentEpoch epoch=0;
    PageIndex page=0;
    std::uint32_t page_count=0;
    std::filesystem::path path;
    std::map<PageIndex,PdfPoint> anchors;
    struct CachedLayout {std::shared_ptr<const PageLayout> layout;std::size_t bytes;std::uint64_t used;};
    std::map<PageIndex,CachedLayout> layouts;
    std::set<PageIndex> requested_layouts;
    std::size_t layout_bytes=0;
    std::uint64_t layout_tick=0;
    std::optional<std::pair<PageIndex,Command>> pending_source_command;
    std::atomic_bool closing{false};
    bool exit_deadline_armed=false;

    explicit Impl(ApplicationController& receiver):owner(receiver),search_timer(&receiver),translate_timer(&receiver),stream_timer(&receiver),position_timer(&receiver){
        owner.Bind(PDF_WAKE,[this](wxThreadEvent&){
            auto delivered=inbox.take();for(auto& e:delivered.events)if(!closing)receive(*e);
            if(closing)return;
            if(delivered.lost_layout){requested_layouts.clear();if(document_state==DocumentState::Ready){request_layout(page);if(scheduled_page)request_layout(*scheduled_page);if(search.active)search_step();}}
            if(delivered.lost_raster)canvas->retry_missing_tiles();
        });
        owner.Bind(IO_WAKE,[this](wxThreadEvent&){for(auto& event:io_inbox.take())if(!closing)receive(*event);});
        owner.Bind(wxEVT_TIMER,[this](wxTimerEvent&){if(!closing)begin_search(false,false);},search_timer.GetId());
        owner.Bind(wxEVT_TIMER,[this](wxTimerEvent&){if(!closing)begin_translation();},translate_timer.GetId());
        // Drain one bounded batch per tick so streamed tokens cannot keep the
        // GUI in a pending-event loop ahead of native close or input events.
        owner.Bind(wxEVT_TIMER,[this](wxTimerEvent&){
            if(closing)return;
            for(const auto& event:inference_inbox.take())receive(*event);
            if(translation_dirty)refresh_translation();
        },stream_timer.GetId());
        owner.Bind(wxEVT_TIMER,[this](wxTimerEvent&){if(!closing){save_reading_position();update_window_title();}},position_timer.GetId());
        pdf=std::make_unique<PdfService>([this](auto event){
            if(closing||event->document!=wanted_epoch.load())return;
            if(inbox.push(std::move(event)))wxQueueEvent(&owner,new wxThreadEvent(PDF_WAKE));
        },native_path(wxStandardPaths::Get().GetResourcesDir()));
        inference=std::make_unique<InferenceService>([this](auto event){
            if(closing||event->model!=wanted_model.load())return;
            inference_inbox.push(std::move(event));
        });
    }
    ~Impl(){
        closing=true;
        inference_inbox.stop();
        native_shortcuts.reset();pdf->stop();inference->stop();if(io)io->stop();
        if(!exit_deadline_armed){arm_exit_deadline();exit_deadline_armed=true;}
        if(io)io->join();
        pdf->join();inference->join();
    }
    void close_window(){
        if(closing)return;
        closing=true;
        inference_inbox.stop();
        translate_timer.Stop();stream_timer.Stop();position_timer.Stop();cancel_search(false);
        native_shortcuts.reset();pdf->stop();inference->stop();if(io)io->stop();
        arm_exit_deadline();exit_deadline_armed=true;
        frame->Hide();frame->Destroy();wxTheApp->ExitMainLoop();
    }
    bool can(Command command) const {
        if(closing)return false;
        if(command==Command::Quit)return modal_depth==0;
        if(command!=Command::Copy&&command!=Command::SelectAll&&(modal_depth||input_composing(wxWindow::FindFocus())))return false;
        switch(command){
        case Command::Open:return !pdf->done();
        case Command::Close:return document_state!=DocumentState::Empty;
        case Command::Previous:return document_state==DocumentState::Ready&&page>0;
        case Command::Next:return document_state==DocumentState::Ready&&page+1<page_count;
        case Command::GoPage:case Command::FocusPage:case Command::FitWidth:case Command::FitPage:case Command::Zoom:
        case Command::Left:case Command::Right:case Command::CopySource:
        case Command::Find:case Command::FindNext:case Command::FindPrevious:return document_state==DocumentState::Ready;
        case Command::Copy:
            if(wxWindow::FindFocus()==canvas)return canvas->has_selection();
            if(auto* text=wxDynamicCast(wxWindow::FindFocus(),wxTextCtrl))return text->CanCopy();
            return false;
        default:return true;
        }
    }
    int show_modal(wxDialog& dialog){ModalScope scope(modal_depth);return dialog.ShowModal();}
    void record(DiagnosticCode code,double seconds=0,std::uint32_t outcome=0){
        DiagnosticRecord r;r.code=code;r.seconds=seconds;r.outcome=outcome;r.metal=backend=="Metal";
        r.context=inference_runtime.context;r.batch=inference_runtime.batch;r.micro_batch=inference_runtime.micro_batch;
        r.threads=static_cast<std::uint32_t>(std::max(0,inference_runtime.threads));r.page=page+1;
        if(recent_diagnostics.size()>=64)recent_diagnostics.pop_front();recent_diagnostics.push_back(diagnostic_line(r));
    }
    std::string diagnostics()const{
        std::ostringstream out;out<<product::name<<" "<<product::version<<"\nBuild: "<<product::preset
            <<"\nSource: "<<build::revision<<(build::dirty?" (modified)":"")<<"\nBuild identity: "<<build::identity
            <<"\nOS: "<<wxGetOsDescription().ToStdString(wxConvUTF8)<<"\nPDFium: "<<product::pdfium_revision
            <<"\nwxWidgets: "<<product::wx_revision<<"\nllama.cpp: "<<product::llama_revision
            <<"\nSQLite: "<<product::sqlite_version<<"\nModel profile: "<<model_profile.id<<"\nSupported metadata: plamo3 / tokenizer plamo3 / translate-0.8b"
            <<"\nBackend: "<<(backend.empty()?"unloaded":backend)<<"\nContext: "<<inference_runtime.context<<"\nThreads: "<<inference_runtime.threads
            <<" / batch threads: "<<inference_runtime.batch_threads<<"\nBatch: "<<inference_runtime.batch<<" / micro batch: "<<inference_runtime.micro_batch
            <<"\nModel load seconds: "<<model_load_seconds<<"\nLast chunk seconds: "<<last_chunk_seconds
            <<"\nStorage warning: "<<(storage_warning?"yes":"no")<<"\n\nRecent bounded diagnostic codes (no paths or document/model text):\n";
        for(const auto& line:recent_diagnostics)out<<line;return out.str();
    }
    void text_dialog(const wxString& title,std::string_view value,bool select_all=false){
        wxDialog dialog(frame,wxID_ANY,title,wxDefaultPosition,frame->FromDIP(wxSize(850,650)),wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto* root=new wxBoxSizer(wxVERTICAL);
        long style=wxTE_MULTILINE|wxTE_READONLY;
#ifdef __WXMSW__
        style|=wxTE_RICH2;
#endif
        auto* text=new wxTextCtrl(&dialog,wxID_ANY,{},wxDefaultPosition,wxDefaultSize,style);text->SetName(title);text->SetMaxLength(0);
        auto font=translation->GetFont();font.SetPointSize(12);text->SetFont(font);text->ChangeValue(wxString::FromUTF8(value.data(),value.size()));
        root->Add(text,1,wxALL|wxEXPAND,frame->FromDIP(8));auto* row=new wxBoxSizer(wxHORIZONTAL);
        auto* copy=new wxButton(&dialog,wxID_COPY,wxS("選択範囲をコピー"));row->Add(copy,0,wxALL,frame->FromDIP(4));row->AddStretchSpacer();
        auto* close=new wxButton(&dialog,wxID_CANCEL,wxS("閉じる"));close->SetDefault();row->Add(close,0,wxALL,frame->FromDIP(4));root->Add(row,0,wxEXPAND);
        copy->Bind(wxEVT_BUTTON,[text](wxCommandEvent&){if(text->CanCopy())text->Copy();});
        dialog.SetSizer(root);dialog.CentreOnParent();text->SetFocus();if(select_all)text->SelectAll();else{text->SetSelection(0,0);text->ShowPosition(0);}show_modal(dialog);
    }
    void open_model_license(){
        wxString text;bool read=false;
        {
            wxLogNull quiet;
            wxFile file(model_license_path());
            read=file.IsOpened()&&file.ReadAll(&text,wxConvUTF8);
        }
        if(!read){
            ModalScope scope(modal_depth);
            wxMessageBox(wxS("ライセンスファイルを開けません。アプリと同じフォルダーに PLaMo-Community-ja.txt を置いてください。"),
                         wxS("PLaMoコミュニティライセンス契約"),wxOK|wxICON_ERROR,frame);
            return;
        }
        const auto bytes=text.ToUTF8();
        text_dialog(wxS("PLaMoコミュニティライセンス契約"),std::string_view(bytes.data(),bytes.length()));
    }
    bool handle_shortcut(ShortcutKey key,ShortcutContext state){
        state.modal=modal_depth>0;state.search_open=search_bar->IsShown();
        auto run=[&](Command command){if(!can(command))return false;execute(command);return true;};
        switch(frame_shortcut(key,state)){
        case ShortcutAction::ModelSettings:return run(Command::ModelSettings);
        case ShortcutAction::FocusPage:return run(Command::FocusPage);case ShortcutAction::CloseSearch:return run(Command::CloseSearch);
        case ShortcutAction::CycleFocus:{
            wxWindow* right_target=welcome->IsShown()?static_cast<wxWindow*>(welcome_license):translation;
            wxWindow* targets[]{canvas,right_target,open_button};auto* focus=wxWindow::FindFocus();int index=2;
            if(focus==canvas||(focus&&left_container->IsDescendant(focus)))index=0;
            else if(focus==translation||(focus&&welcome->IsDescendant(focus)))index=1;
            targets[(index+(state.shift?2:1))%3]->SetFocus();return true;
        }
        case ShortcutAction::Native:return false;
        }return false;
    }
    const PageAttempt* current_attempt()const{
        if(attempt&&attempt_context.page==page)return attempt.get();
        return nullptr;
    }
    void show_translation(std::string text,bool reset=true){
        if(text==shown_translation&&!reset)return;
        update_readonly_text(*translation,shown_translation,text,reset);shown_translation=std::move(text);
    }
    void show_waiting_translation(){
        if(document_state!=DocumentState::Ready)return;
        std::string text;wxString status;
        if(auto layout=current_layout();layout&&
           (layout->availability==PageLayout::Availability::NoTextLayer||layout->availability==PageLayout::Availability::NoTranslatableText||layout->availability==PageLayout::Availability::UnicodeUnavailable||layout->availability==PageLayout::Availability::ResourceLimited)){
            if(layout->availability==PageLayout::Availability::ResourceLimited){text="このページの自動文字抽出はメモリ予算を超えるため停止しました。PDFは引き続き表示できます。";status=wxS("自動文字抽出の容量上限");}
            else if(layout->availability==PageLayout::Availability::UnicodeUnavailable){text="文字のUnicode情報を取得できないため、このページは翻訳できません。PDFは引き続き表示できます。";status=wxS("Unicode情報なし");}
            else{text="翻訳するテキストがありません。PDFは引き続き表示できます。";status=wxS("翻訳テキストなし");}
        }else if(model_state==ModelState::Loading)text="モデルを読み込んでいます。PDFはそのまま読めます。";
        else if(model_state==ModelState::Failed)text="モデルを利用できません。「モデル設定」でGGUFを選び直してください。";
        else if(model_state!=ModelState::Ready)text="翻訳モデルを設定すると、このページの訳文を表示します。";
        else if(auto_translate){text="ページの原文を準備しています…";status=wxS("翻訳待ち");}
        else{text="「翻訳」メニューで「現在ページを自動翻訳」をオンにすると、訳文を表示します。";status=wxS("自動翻訳オフ");}
        show_translation(std::move(text));right_heading->SetLabel(wxS("訳文"));
        frame->SetStatusText(status,2);
    }
    void invalidate_translation(){
        translate_timer.Stop();inference->cancel_job();
        pending_attempt.reset();scheduled_page.reset();
        if(attempt){attempt->interrupt();save_attempt();attempt.reset();}
        translation_dirty=false;
        show_waiting_translation();
    }
    void update_window_title(){
        if(!frame)return;auto title=wxString::FromUTF8(product::name);
        if(document_state!=DocumentState::Empty&&!path.empty())title=display_path(path.filename())+wxS(" — ")+title;
        if(storage_warning)title+=wxS(" [処理エラーあり]");
        if(frame->GetTitle()!=title)frame->SetTitle(title);
    }
    void storage_result(bool accepted){if(!accepted){storage_warning=true;update_window_title();frame->SetStatusText(wxS("処理を受け付けられませんでした。少し待ってから再試行できます。現在の表示は保持しています。"),0);}}
    void save_attempt(){if(io&&attempt)storage_result(io->save_attempt(attempt_context,attempt->snapshot()));}
    void request_position(PageIndex target){
        if(loaded_positions.contains(target)||requested_positions.contains(target))return;
        if(!io||native_document_key.empty()){loaded_positions.insert(target);return;}
        if(io->done()){loaded_positions.insert(target);storage_warning=true;return;}
        if(io->read_page(epoch,document_key,native_document_key,target))requested_positions.insert(target);else{storage_result(false);if(scheduled_page&&*scheduled_page==target)translate_timer.StartOnce(150);}
    }
    void save_reading_position(){
        if(!io||!document_state_loaded||restoring_reading||document_state!=DocumentState::Ready)return;
        ReadingPosition position;position.page=page;position.pdf_anchor=canvas->reading_anchor();
        position.text_anchor=pending_text_anchor&&pending_text_anchor->first==page?pending_text_anchor->second:text_logical_anchor(*translation);
        if(last_saved_position==position)return;
        if(io->save_position(document_key,path,position))last_saved_position=position;
        else storage_result(false);
    }
    void save_preferences(){
        if(!io||!preferences_loaded)return;
        const auto bytes=model_path.u8string();
        std::map<std::string,std::string> values{{"model-path",std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size())},
            {"form",std::string(form_setting(form))},{"auto-translate",auto_translate?"1":"0"},
            {"zoom-mode",std::to_string(static_cast<unsigned>(canvas->zoom_mode()))},{"zoom-percent",std::to_string(canvas->zoom_percent())},{"text-size",std::to_string(text_size)}};
        if(split->GetClientSize().x>0)values["split-ratio"]=std::to_string(static_cast<double>(split->GetSashPosition())/split->GetClientSize().x);
        values["window-maximized"]=frame->IsMaximized()?"1":"0";
        if(!frame->IsMaximized()&&!frame->IsIconized()){
            const auto size=frame->ToDIP(frame->GetSize());const auto position=frame->GetPosition();
            values["window-width"]=std::to_string(size.x);values["window-height"]=std::to_string(size.y);
            values["window-x"]=std::to_string(position.x);values["window-y"]=std::to_string(position.y);
        }
        storage_result(io->settings(std::move(values)));
    }
    void set_text_size(int size){
        const auto anchor=text_logical_anchor(*translation);
        long begin=0,end=0;translation->GetSelection(&begin,&end);
        text_size=size;auto font=translation->GetFont();font.SetPointSize(size);translation->SetFont(font);
        translation->SetSelection(begin,end);restore_text_logical_anchor(*translation,anchor);
    }
    void capture_resize_anchor(){if(!closing&&!resize_text_anchor)resize_text_anchor=ResizeAnchor{epoch,page,text_logical_anchor(*translation)};}
    void restore_after_resize(){
        if(closing||resize_restore_queued)return;resize_restore_queued=true;
        owner.CallAfter([this]{
            resize_restore_queued=false;auto anchor=std::exchange(resize_text_anchor,{});
            if(frame&&!closing&&anchor&&anchor->document==epoch&&anchor->page==page)restore_text_logical_anchor(*translation,anchor->offset);
        });
    }
    void refresh_translation(){
        translation_dirty=false;const auto* shown=current_attempt();if(!shown)return;
        auto text=shown->display_text();wxString status;
        switch(shown->state()){
        case PageState::Complete:status=wxS("完了");break;
        case PageState::Partial:status=wxS("部分結果");break;
        case PageState::Failed:status=wxS("翻訳できませんでした");break;
        case PageState::NoText:
            status=wxS("翻訳するテキストがありません");
            if(text.empty())text="翻訳するテキストがありません。PDFは引き続き表示できます。";
            if(auto layout=current_layout();layout&&layout->availability==PageLayout::Availability::UnicodeUnavailable){status=wxS("文字のUnicode情報を取得できません");text="文字のUnicode情報を取得できないため、このページは翻訳できません。PDFは引き続き表示できます。";}
            break;
        default:status=wxS("翻訳中");if(text.empty())text="翻訳しています…";break;
        }
        show_translation(std::move(text),false);
        if(pending_text_anchor&&pending_text_anchor->first==page&&(shown->state()==PageState::Complete||shown->state()==PageState::Partial||shown->state()==PageState::Failed||shown->state()==PageState::NoText)){
            restore_text_logical_anchor(*translation,pending_text_anchor->second);pending_text_anchor.reset();
        }
        const auto heading=wxS("訳文 — ")+status;
        if(right_heading->GetLabel()!=heading){
            right_heading->SetLabel(heading);
            right_heading->GetParent()->Layout();
        }
        frame->SetStatusText(status,2);
    }
    void schedule_translation(){
        if(closing||!auto_translate||document_state!=DocumentState::Ready||model_state!=ModelState::Ready)return;
        scheduled_page=page;translate_timer.StartOnce(150);
    }
    void pump_translation(){
        if(closing||!attempt||model_state!=ModelState::Ready)return;
        if(auto request=attempt->next_request()){
            if(!inference->plan(*request))attempt->preparation_failed(request->stamp,FinishReason::ModelUnavailable);
        }
        translation_dirty=true;
    }
    void begin_translation(){
        if(closing||document_state!=DocumentState::Ready||model_state!=ModelState::Ready)return;
        if(!auto_translate)return;
        const auto target=page;
        auto found=layouts.find(target);auto layout=found==layouts.end()?nullptr:found->second.layout;
        if(!layout){request_layout(target);return;}
        if(!loaded_positions.contains(target)){request_position(target);return;}
        if(layout->availability==PageLayout::Availability::ResourceLimited){
            scheduled_page.reset();show_waiting_translation();return;
        }
        scheduled_page.reset();
        if(pending_attempt)return;
        if(attempt&&attempt_context.page==target){
            if(attempt->state()==PageState::Running)return;
            if(attempt->state()!=PageState::Complete&&attempt->state()!=PageState::NoText)attempt->resume();pump_translation();refresh_translation();return;
        }
        if(attempt){
            attempt->interrupt();save_attempt();inference->cancel_job();
            attempt.reset();
        }
        auto source=PageAttempt::source(*layout);
        CacheContext context{document_key,model_key,profile_digest(model_profile,form,model_capacity),
            page_source_digest(*layout),target};
        WorkStamp stamp;stamp.document=epoch;stamp.page=target;stamp.model=model_epoch;stamp.source=1;stamp.settings=settings_revision;
        attempt.reset();
        if(io){
            pending_attempt=PendingAttempt{++next_io_request,stamp,context,std::move(source)};
            if(io->read_attempt(context,pending_attempt->request,epoch)){frame->SetStatusText(wxS("この起動中の訳文を確認しています…"),2);return;}
            source=std::move(pending_attempt->source);pending_attempt.reset();storage_result(false);
        }
        attempt_context=context;
        attempt=std::make_unique<PageAttempt>(stamp,context.condition_key(),std::move(source),form);
        save_attempt();pump_translation();refresh_translation();
    }
    void load_model(){
        if(inference->done()){model_state=ModelState::Failed;frame->SetStatusText(wxS("推論処理が終了しました。アプリを再起動してください。"),1);return;}
        invalidate_translation();
        model_state=ModelState::Loading;model_error.clear();backend.clear();wanted_model=++model_epoch;
        show_waiting_translation();
        model_key="provisional:model:"+new_uuid();save_preferences();
        frame->SetStatusText(wxS("モデル読込中…"),1);inference->load(model_epoch,model_path,BackendPreference::Automatic);
    }
    void load_startup_model(){
        if(model_state!=ModelState::Unconfigured||!model_path.empty())return;
        const auto directory=application_directory();
        if(const auto bundled=bundled_model(directory))model_path=*bundled;
        else{
            wxFileDialog file(frame,wxS("同梱GGUFが見つかりません。モデルを選択してください"),display_path(directory),
                wxString::FromUTF8(bundled_model_filename),wxS("GGUF (*.gguf)|*.gguf|すべてのファイル|*"),wxFD_OPEN|wxFD_FILE_MUST_EXIST);
            if(show_modal(file)!=wxID_OK||closing)return;
            model_path=native_path(file.GetPath());
        }
        load_model();
    }
    void model_settings(){
        wxDialog dialog(frame,wxID_ANY,wxS("モデル設定"),wxDefaultPosition,frame->FromDIP(wxSize(720,250)),wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto root=new wxBoxSizer(wxVERTICAL);const auto gap=frame->FromDIP(8);
        auto description=new wxStaticText(&dialog,wxID_ANY,wxS("起動時はアプリと同じフォルダーの同梱GGUFを自動で読み込みます。\nここでは別のPLaMo 3 Translate 0.8B GGUFを選択できます。利用条件は同梱のPLaMoライセンスで確認してください。"));
        root->Add(description,0,wxALL|wxEXPAND,gap);
        if(!model_error.empty()){
            auto failure=new wxStaticText(&dialog,wxID_ANY,wxString::FromUTF8(model_error));failure->Wrap(frame->FromDIP(660));root->Add(failure,0,wxALL|wxEXPAND,gap);
        }
        auto row=new wxBoxSizer(wxHORIZONTAL);auto path_field=new wxTextCtrl(&dialog,wxID_ANY,display_path(model_path));path_field->SetName(wxS("GGUFファイル"));row->Add(path_field,1,wxALL|wxEXPAND,gap);
        auto browse=new wxButton(&dialog,wxID_ANY,wxS("ファイルを選択…"));row->Add(browse,0,wxALL,gap);root->Add(row,0,wxEXPAND);
        browse->Bind(wxEVT_BUTTON,[&](wxCommandEvent&){wxFileDialog file(&dialog,wxS("GGUFを選択"),{}, {},wxS("GGUF (*.gguf)|*.gguf|すべてのファイル|*"),wxFD_OPEN|wxFD_FILE_MUST_EXIST);if(show_modal(file)==wxID_OK)path_field->ChangeValue(file.GetPath());});
        auto* terms=new wxButton(&dialog,wxID_HELP,wxS("ライセンス・利用条件"));root->Add(terms,0,wxALL,gap);
        terms->Bind(wxEVT_BUTTON,[&](wxCommandEvent&){text_dialog(wxS("ライセンス・利用条件"),resources::licenses());});
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxALL|wxALIGN_RIGHT,gap);dialog.SetSizer(root);dialog.CentreOnParent();
        if(show_modal(dialog)!=wxID_OK||path_field->GetValue().empty())return;
        model_path=native_path(path_field->GetValue());load_model();
    }
    void receive(const InferenceEvent& event){
        if(event.model!=model_epoch)return;
        switch(event.kind){
        case InferenceEvent::Kind::ModelReady:
            model_state=ModelState::Ready;backend=event.backend;model_capacity=event.context_capacity;model_profile=event.profile;
            inference_runtime=event.runtime;model_load_seconds=event.seconds;record(DiagnosticCode::ModelReady,event.seconds);
            if(!current_attempt())show_waiting_translation();
            if(event.file)model_key="provisional:model:"+provisional_file_key(event.file->initial_signature());
            if(io&&event.file)storage_result(io->track_model(model_epoch,model_key,model_path,event.file));
            frame->SetStatusText(wxS("モデル準備完了・")+wxString::FromUTF8(backend),1);
            schedule_translation();break;
        case InferenceEvent::Kind::ModelFailed:
            model_state=ModelState::Failed;model_error=event.detail;
            record(DiagnosticCode::ModelFailed);
            if(!current_attempt())show_waiting_translation();
            frame->SetStatusText(wxString::FromUTF8(event.message),1);
            if(attempt){attempt->interrupt();translation_dirty=true;}
            break;
        case InferenceEvent::Kind::Prepared:
            if(attempt&&attempt->prepared(event.chunk)){
                if(auto prepared=attempt->active_prepared()){
                    if(!io||!io->lookup(attempt_context,prepared)){
                        if(!inference->execute(prepared))attempt->preparation_failed(prepared->stamp,FinishReason::ModelUnavailable);
                    }
                }else{save_attempt();pump_translation();}
                translation_dirty=true;
            }break;
        case InferenceEvent::Kind::PreparationFailed:
            if(attempt&&attempt->preparation_failed(event.stamp,event.result.reason)){save_attempt();pump_translation();refresh_translation();}break;
        case InferenceEvent::Kind::TextDelta:
            if(attempt&&attempt->delta(event.stamp,event.delta))translation_dirty=true;break;
        case InferenceEvent::Kind::Finished:
            if(attempt&&attempt->finished(event.stamp,event.result)){
                last_chunk_seconds=event.seconds;record(DiagnosticCode::ChunkFinished,event.seconds,static_cast<std::uint32_t>(event.result.reason));
                save_attempt();
                if(event.result.reason==FinishReason::ModelUnavailable){model_state=ModelState::Failed;model_error=event.detail;frame->SetStatusText(wxS("モデルを利用できません。モデル設定から読み直してください。"),1);}
                else pump_translation();refresh_translation();
            }break;
        default:break;
        }
    }
    void promote_document_key(const std::string& from,const std::string& key){
        if(document_key!=from)return;document_key=key;
        if(attempt){attempt_context.document_key=key;attempt->promote_condition_key(attempt_context.condition_key());}
        if(pending_attempt)pending_attempt->context.document_key=key;
    }
    void receive(const IoEvent& event){
        switch(event.kind){
        case IoEvent::Kind::Initialized:{
            preferences_loaded=true;
            record(DiagnosticCode::SessionStarted);save_preferences();
            frame->SetStatusText(wxS("文書データと設定はこの起動中だけメモリ内に保持します。"),0);
            break;
        }
        case IoEvent::Kind::DocumentLoaded:
            if(event.document!=epoch||!event.saved_document)break;
            promote_document_key(event.from_key,event.key);document_state_loaded=true;
            loaded_positions.insert(event.saved_document->position.page);
            if(!reading_touched){
                const auto& position=event.saved_document->position;
                if(position.page<page_count){restoring_reading=true;go(position.page);restoring_reading=false;}
                if(position.pdf_anchor){anchors[page]=*position.pdf_anchor;canvas->restore_anchor(*position.pdf_anchor);}
                pending_text_anchor=std::pair{page,position.text_anchor};
            }
            schedule_translation();break;
        case IoEvent::Kind::PageLoaded:{
            if(event.document!=epoch||!event.saved_document)break;
            const auto target=event.saved_document->position.page;requested_positions.erase(target);loaded_positions.insert(target);
            if(target==page&&!page_position_touched){
                if(event.saved_document->position.pdf_anchor)canvas->restore_anchor(*event.saved_document->position.pdf_anchor);
                pending_text_anchor=std::pair{target,event.saved_document->position.text_anchor};
            }
            for(auto i=loaded_positions.begin();loaded_positions.size()>2&&i!=loaded_positions.end();){
                if(*i!=page&&(!attempt||*i!=attempt_context.page)&&*i!=target){i=loaded_positions.erase(i);}else ++i;
            }
            if(target==page){
                if(pending_source_command&&pending_source_command->first==page){const auto command=pending_source_command->second;pending_source_command.reset();execute(command);}
                schedule_translation();
            }
            if(scheduled_page&&*scheduled_page==target)translate_timer.StartOnce(1);
            if(!event.detail.empty()){storage_warning=true;frame->SetStatusText(wxS("この起動中の読書位置を読み込めませんでした。"),0);}break;
        }
        case IoEvent::Kind::Fingerprint:
            if(event.model_fingerprint){
                if(event.model!=model_epoch||event.from_key!=model_key)break;model_key=event.key;
                if(attempt){attempt_context.model_key=event.key;attempt->promote_condition_key(attempt_context.condition_key());}
                if(pending_attempt)pending_attempt->context.model_key=event.key;
            }else if(event.document==epoch)promote_document_key(event.from_key,event.key);
            break;
        case IoEvent::Kind::AttemptLoaded:
            if(event.document!=epoch||!pending_attempt||pending_attempt->request!=event.request)break;
            attempt_context=pending_attempt->context;
            attempt=std::make_unique<PageAttempt>(pending_attempt->stamp,attempt_context.condition_key(),std::move(pending_attempt->source),form,event.attempt);
            pending_attempt.reset();save_attempt();pump_translation();refresh_translation();break;
        case IoEvent::Kind::CacheResult:
            if(!attempt||!attempt->accepts(event.stamp))break;
            if(event.cached_text){attempt->finished(event.stamp,{FinishReason::Completed,*event.cached_text,0});save_attempt();pump_translation();refresh_translation();}
            else if(auto prepared=attempt->active_prepared()){
                if(!inference->execute(prepared)){attempt->preparation_failed(event.stamp,FinishReason::ModelUnavailable);save_attempt();pump_translation();}
            }break;
        case IoEvent::Kind::Error:storage_warning=true;record(DiagnosticCode::StorageWarning);frame->SetStatusText(wxString::FromUTF8(event.message),0);break;
        default:break;
        }
    }
    void cancel_search(bool clear_hit=true){
        ++search.generation;search.active=false;search_timer.Stop();
        if(pdf)pdf->cancel_search(epoch,search.generation);
        if(clear_hit){search.hit.reset();canvas->show_search_hit({});}
    }
    void search_step(){
        if(!search.active||closing||document_state!=DocumentState::Ready)return;
        std::shared_ptr<const PageLayout> cached;
        if(auto found=layouts.find(search.cursor);found!=layouts.end())cached=found->second.layout;
        pdf->search({epoch,search.cursor,search.generation,search.query,search.start,search_case->GetValue(),search.backward,std::move(cached)});
        search_label->SetLabel(wxString::Format(wxS("検索中… %u/%u"),search.cursor+1,page_count));left_container->Layout();
    }
    void begin_search(bool backward,bool continuation){
        if(!can(Command::Find))return;
        const auto query=search_input->GetValue().ToStdString(wxConvUTF8);
        const bool resume=continuation&&query==search.query&&search.hit&&search.hit->first==page;
        ++search.generation;search.query=query;search.backward=backward;search.cursor=page;search.skipped_page=false;
        search.start=resume?(backward?search.hit->second.begin:search.hit->second.end):
            backward?std::numeric_limits<std::size_t>::max():0;
        search.remaining=page_count+(resume?1U:0U);search.active=!query.empty();
        if(!resume){search.hit.reset();canvas->show_search_hit({});}
        if(search.active)search_step();else search_label->SetLabel(wxString{});
    }
    void show_search(){
        if(!search_bar->IsShown())search_return_focus=wxWindow::FindFocus();
        search_bar->Show();left_container->Layout();search_input->SetFocus();search_input->SelectAll();
    }
    void request_layout(PageIndex index){
        if(!layouts.contains(index)&&!requested_layouts.contains(index)){
            if(pdf->extract(epoch,index))requested_layouts.insert(index);
            else if(scheduled_page&&*scheduled_page==index)translate_timer.StartOnce(150);
        }
    }
    std::shared_ptr<const PageLayout> current_layout(){
        auto found=layouts.find(page);if(found==layouts.end())return {};
        found->second.used=++layout_tick;return found->second.layout;
    }
    void cache_layout(PageIndex index,std::shared_ptr<const PageLayout> layout){
        const auto bytes=layout_memory_bytes(*layout);
        if(bytes>32U*1024*1024)return;
        if(auto old=layouts.find(index);old!=layouts.end())layout_bytes-=old->second.bytes;
        layouts.insert_or_assign(index,CachedLayout{std::move(layout),bytes,++layout_tick});layout_bytes+=bytes;
        while(layout_bytes>32U*1024*1024&&layouts.size()>1){
            auto victim=layouts.end();
            for(auto i=layouts.begin();i!=layouts.end();++i)if(i->first!=page&&(victim==layouts.end()||i->second.used<victim->second.used))victim=i;
            if(victim==layouts.end())break;layout_bytes-=victim->second.bytes;layouts.erase(victim);
        }
    }
    void route(int id,Command command){
        frame->Bind(wxEVT_MENU,[this,command](wxCommandEvent&){execute(command);},id);
        frame->Bind(wxEVT_BUTTON,[this,command](wxCommandEvent&){execute(command);},id);
        frame->Bind(wxEVT_UPDATE_UI,[this,command](wxUpdateUIEvent& e){e.Enable(can(command));},id);
    }
    void headings(){
        const bool empty=document_state==DocumentState::Empty;
        auto* focus=wxWindow::FindFocus();
        const bool right_focused=focus==translation||(focus&&welcome->IsDescendant(focus));
        welcome->Show(empty);translation->Show(!empty);
        if(right_focused){
            wxWindow* target=empty?static_cast<wxWindow*>(welcome_license):translation;
            target->SetFocus();
        }
        left_heading->SetLabel(page_count?wxS("原文 —"):wxS("原文"));
        right_heading->SetLabel(wxS("訳文"));
        page_input->ChangeValue(page_count?wxString::Format("%u",page+1):wxString{});
        page_input->Show(page_count!=0);
        page_count_label->SetLabel(wxString::Format(wxS("/ %u ページ"),page_count));
        page_count_label->Show(page_count!=0);
        frame->SetStatusText(page_count?wxString{}:wxS("PDFを開いてください。"),0);
        frame->SetStatusText({},2);
        left_container->Layout();
        right_heading->GetParent()->Layout();
    }
    void remember_document(){
        const auto bytes=path.u8string();const std::string name(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        std::erase(recent_paths,name);recent_paths.insert(recent_paths.begin(),name);
        if(recent_paths.size()>20)recent_paths.resize(20);
        while(recent_menu->GetMenuItemCount())recent_menu->Destroy(recent_menu->FindItemByPosition(0));
        for(std::size_t index=0;index<recent_paths.size();++index)
            recent_menu->Append(wxID_HIGHEST+800+static_cast<int>(index),wxString::FromUTF8(recent_paths[index]));
    }
    void open_path(const wxString& name){
        if(closing||modal_depth)return;
        std::filesystem::path next_path;
        try{
            wxFileName absolute(name);
            if(name.empty()||!absolute.MakeAbsolute())throw std::invalid_argument("Invalid PDF path");
            next_path=native_path(absolute.GetFullPath());
            if(next_path.empty())throw std::invalid_argument("Empty PDF path");
        }catch(const std::exception&){
            frame->SetStatusText(wxS("PDFのパスを読み取れません。ファイルを選び直してください。"),0);return;
        }
        save_reading_position();if(io)io->close_document();
        invalidate_translation();
        document_key="provisional:document:"+new_uuid();native_document_key.clear();document_state_loaded=false;reading_touched=false;page_position_touched=false;pending_text_anchor.reset();last_saved_position.reset();loaded_positions.clear();requested_positions.clear();
        path=std::move(next_path);document_state=DocumentState::Opening;page=0;page_count=0;
        cancel_search();search_input->ChangeValue(wxString{});search.query.clear();search_label->SetLabel(wxString{});
        anchors.clear();layouts.clear();layout_bytes=0;requested_layouts.clear();pending_source_command.reset();wanted_epoch=++epoch;
        canvas->set_page(epoch,0);
        show_translation("PDFを読み込んでいます…");
        frame->SetTitle(display_path(path.filename())+wxS(" — ")+wxString::FromUTF8(product::name));
        headings();frame->SetStatusText(wxS("PDFを読み込んでいます…"),0);
        pdf->open(epoch,path);
    }
    void go(PageIndex target,bool search_navigation=false){
        if(target>=page_count||target==page)return;
        if(!restoring_reading)reading_touched=true;save_reading_position();last_saved_position.reset();
        page_position_touched=false;pending_text_anchor.reset();
        invalidate_translation();
        if(!search_navigation)cancel_search();
        if(auto a=canvas->reading_anchor())anchors[page]=*a;
        page=target;canvas->set_page(epoch,page);
        pending_source_command.reset();if(auto layout=current_layout())canvas->set_layout(std::move(layout));
        headings();show_waiting_translation();pdf->page_info(epoch,page);request_position(page);schedule_translation();
    }
    void receive(const PdfEvent& event){
        if(event.document!=epoch)return;
        switch(event.kind){
        case PdfEvent::Kind::Opened:
            document_state=DocumentState::Ready;page_count=event.page_count;page=0;
            record(DiagnosticCode::PdfOpened);
            remember_document();
            native_document_key=provisional_file_key(event.file_signature);
            document_key="provisional:document:"+native_document_key;
            if(io&&event.file_snapshot)storage_result(io->open_document(epoch,document_key,path,event.file_snapshot));
            canvas->set_page(epoch,0,event.info);headings();
            if(!wxWindow::FindFocus()||wxWindow::FindFocus()==frame||wxWindow::FindFocus()==open_button)canvas->SetFocus();
            show_waiting_translation();schedule_translation();
            break;
        case PdfEvent::Kind::PageInfo:
            if(event.page==page){
                canvas->set_info(event.info);if(auto i=anchors.find(page);i!=anchors.end())canvas->restore_anchor(i->second);
                if(search.hit&&search.hit->first==page)canvas->show_search_hit(search.hit->second);
            }
            break;
        case PdfEvent::Kind::ViewReady:case PdfEvent::Kind::TileReady:
            canvas->accept(event);if(event.kind==PdfEvent::Kind::TileReady&&event.page==page)request_layout(page);break;
        case PdfEvent::Kind::Extracted:
            requested_layouts.erase(event.page);
            if(event.layout){
                cache_layout(event.page,event.layout);
                if(event.page==page){
                    canvas->set_layout(event.layout);
                    if(!current_attempt()&&(event.layout->availability==PageLayout::Availability::NoTextLayer||event.layout->availability==PageLayout::Availability::NoTranslatableText))
                        show_translation("翻訳するテキストがありません。PDFは引き続き表示できます。");
                    else if(!current_attempt()&&event.layout->availability==PageLayout::Availability::UnicodeUnavailable)
                        show_translation("文字のUnicode情報を取得できないため、このページは翻訳できません。PDFは引き続き表示できます。");
                    else if(!current_attempt()&&event.layout->availability==PageLayout::Availability::ResourceLimited)
                        show_waiting_translation();
                    if(pending_source_command&&pending_source_command->first==page){const auto command=pending_source_command->second;pending_source_command.reset();execute(command);}
                    schedule_translation();
                }
                if(scheduled_page&&*scheduled_page==event.page)translate_timer.StartOnce(1);
            }
            break;
        case PdfEvent::Kind::ExtractionDeferred:
            requested_layouts.erase(event.page);if(scheduled_page&&*scheduled_page==event.page)translate_timer.StartOnce(150);break;
        case PdfEvent::Kind::SearchResult:
            if(event.layout)cache_layout(event.page,event.layout);
            if(!search.active||event.query_generation!=search.generation||event.page!=search.cursor)break;
            if(event.layout&&event.layout->availability==PageLayout::Availability::ResourceLimited)search.skipped_page=true;
            if(event.hit){
                search.active=false;search.hit=std::pair{event.page,*event.hit};
                go(event.page,true);if(auto layout=current_layout())canvas->set_layout(layout);
                canvas->show_search_hit(event.hit);search_label->SetLabel(wxString::Format(wxS("%uページ"),event.page+1));
            }else if(search.remaining>1){
                --search.remaining;
                search.cursor=search.backward?(search.cursor==0?page_count-1:search.cursor-1):(search.cursor+1)%page_count;
                search.start=search.backward?std::numeric_limits<std::size_t>::max():0;search_step();
            }else{search.active=false;search.hit.reset();canvas->show_search_hit({});search_label->SetLabel(search.skipped_page?
                wxS("検索できた範囲にはありません。文字情報が多く検索を省略したページがあります。"):wxS("見つかりませんでした。"));}
            left_container->Layout();break;
        case PdfEvent::Kind::PasswordRequired:{
            PdfPasswordDialog dialog(frame,wxS("PDFのパスワードを入力してください。"),wxS("パスワード付きPDF"));
            if(show_modal(dialog)==wxID_OK)dialog.submit(*pdf,epoch,path);
            else execute(Command::Close);
            break;
        }
        case PdfEvent::Kind::Failed:
            record(DiagnosticCode::PdfFailed);
            requested_layouts.erase(event.page);
            if(pending_source_command&&pending_source_command->first==event.page)pending_source_command.reset();
            if(event.code=="PdfSearch"&&event.query_generation==search.generation){search.active=false;search_label->SetLabel(wxS("検索できませんでした。PDFを再度開けます。"));}
            if(event.code=="PdfOpen"){document_state=DocumentState::Failed;show_translation(event.message);}
            if(event.code=="PdfService"){invalidate_translation();document_state=DocumentState::Failed;show_translation(event.message);}
            if(event.code=="PdfPage"&&scheduled_page&&*scheduled_page==event.page){
                scheduled_page.reset();
            }
            if(event.code=="PdfOpen"||event.page==page)frame->SetStatusText(wxString::FromUTF8(event.message),0);
            break;
        default:break;
        }
    }
    void execute(Command command){
        if(!can(command))return;
        switch(command){
        case Command::Open:{
            wxFileDialog dialog(frame,wxS("PDFを開く"),{}, {},wxS("PDFファイル (*.pdf)|*.pdf|すべてのファイル|*"),wxFD_OPEN|wxFD_FILE_MUST_EXIST);
            if(show_modal(dialog)==wxID_OK)open_path(dialog.GetPath());break;
        }
        case Command::Close:
            save_reading_position();if(io)io->close_document();document_state_loaded=false;last_saved_position.reset();
            invalidate_translation();
            loaded_positions.clear();requested_positions.clear();
            cancel_search();search_input->ChangeValue(wxString{});search.query.clear();search_label->SetLabel(wxString{});
            wanted_epoch=++epoch;pdf->close(epoch);document_state=DocumentState::Empty;page_count=0;page=0;
            anchors.clear();layouts.clear();layout_bytes=0;requested_layouts.clear();pending_source_command.reset();canvas->clear_document();show_translation({});headings();frame->SetTitle(wxString::FromUTF8(product::name));break;
        case Command::Previous:go(page-1);break;case Command::Next:go(page+1);break;
        case Command::FocusPage:page_input->SetFocus();page_input->SelectAll();break;
        case Command::GoPage:{
            long value=0;auto text=page_input->GetValue();
            if(text.ToLong(&value)&&value>=1&&static_cast<unsigned long>(value)<=page_count)go(static_cast<PageIndex>(value-1));
            else frame->SetStatusText(wxS("有効なページ番号を入力してください。"),0);
            page_input->ChangeValue(wxString::Format("%u",page+1));break;
        }
        case Command::FitWidth:canvas->set_zoom(ZoomMode::FitWidth);zoom_input->ChangeValue(wxS("幅に合わせる"));break;
        case Command::FitPage:canvas->set_zoom(ZoomMode::FitPage);zoom_input->ChangeValue(wxS("ページ全体"));break;
        case Command::Zoom:{
            auto value=zoom_input->GetValue();
            if(value==wxS("幅に合わせる")){execute(Command::FitWidth);break;}
            if(value==wxS("ページ全体")){execute(Command::FitPage);break;}
            value.Trim(true).Trim(false);if(value.EndsWith("%"))value.RemoveLast();
            double percent=0;
            if(value.ToDouble(&percent)&&std::isfinite(percent)&&percent>0){canvas->set_zoom(ZoomMode::Percent,percent);zoom_input->ChangeValue(wxString::Format("%.1f%%",canvas->zoom_percent()));}
            else {frame->SetStatusText(wxS("正の倍率（%）を入力してください。"),0);zoom_input->ChangeValue(wxString::Format("%.1f%%",canvas->zoom_percent()));}
            break;
        }
        case Command::Left:canvas->rotate(-1);break;case Command::Right:canvas->rotate(1);break;
        case Command::Equal:split->SetSashPosition(split->GetClientSize().x/2);break;
        case Command::TextSize:{
            wxNumberEntryDialog dialog(frame,wxS("訳文の文字サイズをポイントで指定してください。"),wxS("文字サイズ (6〜72)"),wxS("訳文の文字サイズ"),text_size,6,72);
            if(show_modal(dialog)==wxID_OK){set_text_size(static_cast<int>(dialog.GetValue()));save_preferences();}
            break;
        }
        case Command::Copy:case Command::SelectAll:
            if(auto* native=wxDynamicCast(wxWindow::FindFocus(),wxTextCtrl)){if(command==Command::Copy)native->Copy();else native->SelectAll();}
            else if(wxWindow::FindFocus()==canvas){if(command==Command::Copy)canvas->copy_selection();else canvas->select_all();}
            break;
        case Command::CopySource:
            if(auto layout=current_layout()){
                if(!copy_text(layout->reading_text_utf8))frame->SetStatusText(wxS("クリップボードを開けません。もう一度お試しください。"),0);
            }else{pending_source_command=std::pair{page,command};request_layout(page);frame->SetStatusText(wxS("原文を抽出しています…"),0);}
            break;
        case Command::Find:show_search();break;
        case Command::FindNext:case Command::FindPrevious:
            if(!search_bar->IsShown())show_search();
            if(search_input->GetValue().empty()){search_input->SetFocus();break;}
            begin_search(command==Command::FindPrevious,true);break;
        case Command::CloseSearch:
            cancel_search();search_bar->Hide();left_container->Layout();
            if(search_return_focus&&search_return_focus->IsShown()&&search_return_focus->IsEnabled())search_return_focus->SetFocus();else canvas->SetFocus();break;
        case Command::Automatic:
            auto_translate=!auto_translate;save_preferences();if(!current_attempt())show_waiting_translation();if(auto_translate)schedule_translation();break;
        case Command::Plain:case Command::Easy:{
            const auto selected=command==Command::Plain?Form::Plain:Form::Easy;
            if(form!=selected){form=selected;++settings_revision;invalidate_translation();save_preferences();schedule_translation();}break;
        }
        case Command::ModelSettings:model_settings();break;
        case Command::Help:{
            text_dialog(wxS("操作方法"),resources::help());break;
        }
        case Command::Diagnostics:text_dialog(wxS("診断情報をコピー"),diagnostics(),true);break;
        case Command::Licenses:text_dialog(wxS("ライセンス"),resources::licenses());break;
        case Command::About:{
            ModalScope scope(modal_depth);wxMessageBox(wxString::FromUTF8(product::name)+" "+wxString::FromUTF8(product::version)+wxS("\nオフラインPDF翻訳アプリ\nBuilt with PLaMo\nアプリの新規コード: MIT License\n第三者ライブラリ・フォント・モデルの条件は「ライセンス」を参照"),wxS("このアプリについて"),wxOK,frame);break;
        }
        case Command::Quit:frame->Close();break;
        }
    }
    void start(const wxString& initial){
        std::string error;
        const bool font_ok=register_private_font(native_path(wxStandardPaths::Get().GetResourcesDir()),error);
        frame=new wxFrame(nullptr,wxID_ANY,wxString::FromUTF8(product::name));
        const auto icon_data=resources::icon_png();wxMemoryInputStream icon_stream(icon_data.data(),icon_data.size());
        wxImage icon_image(icon_stream,wxBITMAP_TYPE_PNG);if(icon_image.IsOk()){wxIcon icon;icon.CopyFromBitmap(wxBitmap(icon_image));frame->SetIcon(icon);}
        if(font_ok){auto font=frame->GetFont();font.SetFaceName("Noto Sans CJK JP");frame->SetFont(font);}
        auto area=wxDisplay(0U).GetClientArea();auto size=frame->FromDIP(wxSize(1280,860));
        frame->SetSize(wxSize(std::min(size.x,area.width),std::min(size.y,area.height)));frame->Centre();
        frame->SetMinSize(frame->FromDIP(wxSize(640,400)));
        auto file_menu=new wxMenu;file_menu->Append(Open,wxS("PDFを開く…\tCtrl+O"));recent_menu=new wxMenu;file_menu->AppendSubMenu(recent_menu,wxS("この起動中に開いたPDF"));file_menu->Append(Close,wxS("文書を閉じる\tCtrl+W"));
        file_menu->AppendSeparator();file_menu->Append(wxID_EXIT,wxS("終了\tCtrl+Q"));
        frame->Bind(wxEVT_MENU,[this](wxCommandEvent& event){
            const auto index=static_cast<std::size_t>(event.GetId()-(wxID_HIGHEST+800));
            if(index<recent_paths.size())open_path(wxString::FromUTF8(recent_paths[index]));
        },wxID_HIGHEST+800,wxID_HIGHEST+819);
        auto selection_menu=new wxMenu;selection_menu->Append(wxID_COPY,wxS("コピー\tCtrl+C"));selection_menu->Append(wxID_SELECTALL,wxS("全選択\tCtrl+A"));
        selection_menu->AppendSeparator();selection_menu->Append(CopySource,wxS("ページの原文をコピー"));
        selection_menu->AppendSeparator();selection_menu->Append(Find,wxS("PDF内を検索…\tCtrl+F"));selection_menu->Append(FindNext,wxS("次の検索結果"));selection_menu->Append(FindPrevious,wxS("前の検索結果"));
        auto view_menu=new wxMenu;
        view_menu->Append(FitWidth,wxS("幅に合わせる"));view_menu->Append(FitPage,wxS("ページ全体"));
        view_menu->Append(Left,wxS("左回転"));view_menu->Append(Right,wxS("右回転"));view_menu->Append(Equal,wxS("左右を均等にする"));
        view_menu->Append(TextSize,wxS("訳文の文字サイズ…"));
#ifdef __WXMAC__
        view_menu->Append(FocusPage,wxS("ページ番号へ (⌘G)"));
        view_menu->Append(Previous,wxS("前のページ\tCtrl+["));view_menu->Append(Next,wxS("次のページ\tCtrl+]"));
#else
        view_menu->Append(FocusPage,wxS("ページ番号へ (Ctrl+G)"));
        view_menu->Append(Previous,wxS("前のページ\tAlt+PageUp"));view_menu->Append(Next,wxS("次のページ\tAlt+PageDown"));
#endif
        auto help_menu=new wxMenu;help_menu->Append(Help,wxS("操作方法"));help_menu->Append(Diagnostics,wxS("診断情報をコピー…"));help_menu->Append(Licenses,wxS("ライセンス…"));help_menu->Append(About,wxS("このアプリについて"));
        auto translate_menu=new wxMenu;
        translate_menu->AppendCheckItem(Automatic,wxS("現在ページを自動翻訳"));translate_menu->Check(Automatic,true);
        translate_menu->AppendSeparator();translate_menu->AppendRadioItem(Plain,wxS("標準文体"));translate_menu->AppendRadioItem(Easy,wxS("やさしい文体"));translate_menu->Check(Plain,true);
        auto model_menu=new wxMenu;
#ifdef __WXMAC__
        model_menu->Append(ModelSettings,wxS("モデルを選択… (⌘,)"));
#else
        model_menu->Append(ModelSettings,wxS("モデルを選択… (Ctrl+,)"));
#endif
        auto menu=new wxMenuBar;menu->Append(file_menu,wxS("ファイル"));menu->Append(selection_menu,wxS("選択・コピー"));menu->Append(view_menu,wxS("表示"));menu->Append(translate_menu,wxS("翻訳"));menu->Append(model_menu,wxS("モデル設定"));menu->Append(help_menu,wxS("ヘルプ"));frame->SetMenuBar(menu);
        route(Open,Command::Open);route(Close,Command::Close);route(Previous,Command::Previous);route(Next,Command::Next);
        route(GoPage,Command::GoPage);route(FitWidth,Command::FitWidth);route(FitPage,Command::FitPage);route(Zoom,Command::Zoom);
        route(FocusPage,Command::FocusPage);
        route(TextSize,Command::TextSize);
        route(Diagnostics,Command::Diagnostics);route(Licenses,Command::Licenses);
        route(Left,Command::Left);route(Right,Command::Right);route(Equal,Command::Equal);route(Help,Command::Help);route(About,Command::About);
        route(wxID_COPY,Command::Copy);route(wxID_SELECTALL,Command::SelectAll);route(wxID_EXIT,Command::Quit);
        route(CopySource,Command::CopySource);
        route(Find,Command::Find);route(FindNext,Command::FindNext);route(FindPrevious,Command::FindPrevious);route(CloseSearch,Command::CloseSearch);
        route(Automatic,Command::Automatic);
        route(Plain,Command::Plain);route(Easy,Command::Easy);route(ModelSettings,Command::ModelSettings);
        frame->Bind(wxEVT_UPDATE_UI,[this](wxUpdateUIEvent& e){e.Check(auto_translate);},Automatic);
        frame->Bind(wxEVT_UPDATE_UI,[this](wxUpdateUIEvent& e){e.Check(form==Form::Plain);},Plain);
        frame->Bind(wxEVT_UPDATE_UI,[this](wxUpdateUIEvent& e){e.Check(form==Form::Easy);},Easy);
        auto root=new wxBoxSizer(wxVERTICAL);
        auto toolbar=new wxPanel(frame);
        auto tools=new wxBoxSizer(wxHORIZONTAL);
        const int gap=frame->FromDIP(4);
        open_button=new wxButton(toolbar,Open,wxS("PDFを開く"));
        tools->Add(open_button,0,wxALL|wxALIGN_CENTER_VERTICAL,gap);
        toolbar->SetSizer(tools);root->Add(toolbar,0,wxEXPAND);
        split=new wxSplitterWindow(frame,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxSP_LIVE_UPDATE|wxSP_3D);
        auto left=new wxPanel(split);auto right=new wxPanel(split);auto ls=new wxBoxSizer(wxVERTICAL);auto rs=new wxBoxSizer(wxVERTICAL);
        left_container=left;
        left_heading=new wxStaticText(left,wxID_ANY,wxS("原文"));right_heading=new wxStaticText(right,wxID_ANY,wxS("訳文"));
        auto source_header=new wxWrapSizer(wxHORIZONTAL,wxREMOVE_LEADING_SPACES);
        auto page_selector=new wxBoxSizer(wxHORIZONTAL);
        page_selector->Add(left_heading,0,wxALL|wxALIGN_CENTER_VERTICAL,gap);
        page_input=new wxTextCtrl(left,GoPage,{},wxDefaultPosition,frame->FromDIP(wxSize(56,-1)),wxTE_PROCESS_ENTER|wxTE_CENTRE);
        page_input->SetName(wxS("ページ番号"));
        page_selector->Add(page_input,0,wxTOP|wxBOTTOM|wxALIGN_CENTER_VERTICAL,gap);
        page_count_label=new wxStaticText(left,wxID_ANY,{});
        page_selector->Add(page_count_label,0,wxALL|wxALIGN_CENTER_VERTICAL,gap);
        source_header->Add(page_selector,0,wxALIGN_CENTER_VERTICAL);
        wxArrayString options;for(const auto* label:{wxS("幅に合わせる"),wxS("ページ全体"),wxS("50%"),wxS("75%"),wxS("100%"),wxS("125%"),wxS("150%"),wxS("200%"),wxS("400%")})options.Add(label);
        zoom_input=new wxComboBox(left,Zoom,wxS("幅に合わせる"),wxDefaultPosition,frame->FromDIP(wxSize(160,-1)),options,wxTE_PROCESS_ENTER);
        zoom_input->SetName(wxS("PDFの倍率"));
        source_header->Add(zoom_input,0,wxALL|wxALIGN_CENTER_VERTICAL,gap);
        auto translation_header=new wxBoxSizer(wxHORIZONTAL);
        translation_header->Add(right_heading,1,wxALL|wxALIGN_CENTER_VERTICAL,gap);
        const int header_height=source_header->GetMinSize().y;
        source_header->SetMinSize(-1,header_height);
        translation_header->SetMinSize(-1,header_height);
        ls->Add(source_header,0,wxEXPAND);
        rs->Add(translation_header,0,wxEXPAND);
        search_bar=new wxPanel(left);auto search_sizer=new wxWrapSizer(wxHORIZONTAL);
        search_input=new wxTextCtrl(search_bar,wxID_ANY,{},wxDefaultPosition,frame->FromDIP(wxSize(160,-1)),wxTE_PROCESS_ENTER);search_input->SetName(wxS("PDFの検索語"));
        search_sizer->Add(search_input,0,wxALL,gap);
        auto search_button=[&](int id,const wxString& label,Command command){
            auto* button=new wxButton(search_bar,id,label);search_sizer->Add(button,0,wxALL,gap);
            button->Bind(wxEVT_BUTTON,[this,command](wxCommandEvent&){execute(command);});
        };
        search_button(FindPrevious,wxS("前の結果"),Command::FindPrevious);search_button(FindNext,wxS("次の結果"),Command::FindNext);
        search_case=new wxCheckBox(search_bar,wxID_ANY,wxS("英字の大文字・小文字を区別"));search_sizer->Add(search_case,0,wxALL|wxALIGN_CENTER_VERTICAL,gap);
        search_button(CloseSearch,wxS("閉じる"),Command::CloseSearch);
        search_label=new wxStaticText(search_bar,wxID_ANY,{});search_sizer->Add(search_label,0,wxALL|wxALIGN_CENTER_VERTICAL,gap);
        search_bar->SetSizer(search_sizer);ls->Add(search_bar,0,wxEXPAND);search_bar->Hide();
        search_input->Bind(wxEVT_TEXT,[this](wxCommandEvent&){cancel_search();search_timer.StartOnce(150);});
        search_input->Bind(wxEVT_TEXT_ENTER,[this](wxCommandEvent&){begin_search(false,true);});
        search_case->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){cancel_search();begin_search(false,false);});
        auto page_row=new wxBoxSizer(wxHORIZONTAL);
        auto page_arrow=[&](int id,const wxString& arrow,const wxString& name){
            auto* control=new wxButton(left,id,arrow,wxDefaultPosition,frame->FromDIP(wxSize(30,40)),wxBU_EXACTFIT);
            control->SetFont(control->GetFont().Scaled(1.6f));
            control->SetName(name);control->SetToolTip(name);
            page_row->Add(control,0,wxALIGN_CENTER_VERTICAL);
        };
        page_arrow(Previous,wxS("‹"),wxS("前のページ"));
        canvas=new PdfCanvas(left,[this](ViewRequest request){pdf->viewport(std::move(request));});
        page_row->Add(canvas,1,wxEXPAND);
        page_arrow(Next,wxS("›"),wxS("次のページ"));
        ls->Add(page_row,1,wxEXPAND);
        const auto touch=[this]{reading_touched=true;page_position_touched=true;pending_text_anchor.reset();};
        canvas->Bind(wxEVT_MOUSEWHEEL,[touch](wxMouseEvent& e){touch();e.Skip();});
        canvas->Bind(wxEVT_MOTION,[touch](wxMouseEvent& e){if(e.Dragging())touch();e.Skip();});
        canvas->Bind(wxEVT_KEY_DOWN,[touch](wxKeyEvent& e){if(e.GetKeyCode()==WXK_UP||e.GetKeyCode()==WXK_DOWN||e.GetKeyCode()==WXK_LEFT||e.GetKeyCode()==WXK_RIGHT||e.GetKeyCode()==WXK_PAGEUP||e.GetKeyCode()==WXK_PAGEDOWN)touch();e.Skip();});
        for(const auto& type:{wxEVT_SCROLLWIN_LINEUP,wxEVT_SCROLLWIN_LINEDOWN,wxEVT_SCROLLWIN_PAGEUP,wxEVT_SCROLLWIN_PAGEDOWN,wxEVT_SCROLLWIN_THUMBTRACK})canvas->Bind(type,[touch](wxScrollWinEvent& e){touch();e.Skip();});
        long text_style=wxTE_MULTILINE|wxTE_READONLY;
#ifdef __WXMSW__
        text_style|=wxTE_RICH2;
#endif
        translation=new wxTextCtrl(right,wxID_ANY,{},wxDefaultPosition,wxDefaultSize,text_style);translation->SetName(wxS("現在ページの日本語訳"));
#ifdef __WXMSW__
        translation->SetDoubleBuffered(true);
#endif
        translation->SetMaxLength(0);
        translation->Bind(wxEVT_MOUSEWHEEL,[touch](wxMouseEvent& e){touch();e.Skip();});translation->Bind(wxEVT_LEFT_DOWN,[touch](wxMouseEvent& e){touch();e.Skip();});
        auto font=translation->GetFont();if(font_ok)font.SetFaceName("Noto Sans CJK JP");font.SetPointSize(14);translation->SetFont(font);rs->Add(translation,1,wxEXPAND);
        welcome=new wxScrolledWindow(right,wxID_ANY);
        welcome->SetName(wxS("PLaMoについて"));
        welcome->SetBackgroundColour(translation->GetBackgroundColour());
        welcome->SetScrollRate(10,10);
        auto welcome_sizer=new wxBoxSizer(wxVERTICAL);
        welcome_sizer->AddStretchSpacer();
        auto credit=new wxStaticText(welcome,wxID_ANY,wxS("Built with PLaMo"));
        credit->SetFont(credit->GetFont().Scaled(1.6f).Bold());
        welcome_sizer->Add(credit,0,wxALIGN_CENTER_HORIZONTAL|wxALL,frame->FromDIP(16));
        const wxString notice_text=wxS("このアプリは株式会社Preferred Networksが提供するPLaMo-3-translate-0.8b-GGUFを利用しています");
        auto notice=new wxStaticText(welcome,wxID_ANY,notice_text,
            wxDefaultPosition,wxDefaultSize,wxALIGN_CENTRE|wxST_NO_AUTORESIZE);
        notice->SetMinSize(wxSize(1,-1));
        welcome_sizer->Add(notice,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,frame->FromDIP(24));
        welcome->Bind(wxEVT_SIZE,[this,notice,notice_text](wxSizeEvent& event){
            const int width=std::max(1,welcome->GetClientSize().x-welcome->FromDIP(48));
            // wxStaticText::Wrap only breaks at whitespace. Measure each line
            // so Japanese text and the model name fit the available width.
            wxString wrapped,line;
            for(const auto character:notice_text){
                if(!line.empty()&&notice->GetTextExtent(line+character).x>width){
                    wrapped+=line+"\n";line.clear();
                }
                line+=character;
            }
            notice->SetLabel(wrapped+line);
            notice->InvalidateBestSize();
            welcome->Layout();welcome->FitInside();
            event.Skip();
        });
        welcome_license=new wxHyperlinkCtrl(welcome,wxID_ANY,wxS("PLaMoコミュニティライセンス契約"),
            wxFileSystem::FileNameToURL(model_license_path()),wxDefaultPosition,wxDefaultSize,wxHL_ALIGN_CENTRE|wxBORDER_NONE);
        welcome_license->SetName(wxS("PLaMoコミュニティライセンス契約"));
        welcome_license->Bind(wxEVT_HYPERLINK,[this](wxHyperlinkEvent&){open_model_license();});
        welcome_sizer->Add(welcome_license,0,wxALIGN_CENTER_HORIZONTAL|wxLEFT|wxRIGHT|wxBOTTOM,frame->FromDIP(16));
        welcome_sizer->AddStretchSpacer();
        welcome->SetSizer(welcome_sizer);
        rs->Add(welcome,1,wxEXPAND);
        left->SetSizer(ls);right->SetSizer(rs);split->SplitVertically(left,right,frame->GetClientSize().x/2);split->SetSashGravity(0.5);split->SetMinimumPaneSize(frame->FromDIP(200));root->Add(split,1,wxEXPAND);frame->SetSizer(root);
        frame->CreateStatusBar(3);int widths[]{-2,-1,-1};frame->SetStatusWidths(3,widths);frame->SetStatusText(wxS("モデル未設定"),1);
        if(!font_ok)frame->SetStatusText(wxS("同梱フォントを登録できませんでした。"),2);
        frame->SetDropTarget(new PdfDropTarget([this](const wxString& name){open_path(name);}));
        canvas->SetDropTarget(new PdfDropTarget([this](const wxString& name){open_path(name);}));
        page_input->Bind(wxEVT_TEXT_ENTER,[this](wxCommandEvent&){execute(Command::GoPage);});
        page_input->Bind(wxEVT_KILL_FOCUS,[this](wxFocusEvent& e){if(page_count)execute(Command::GoPage);e.Skip();});
        zoom_input->Bind(wxEVT_TEXT_ENTER,[this](wxCommandEvent&){execute(Command::Zoom);});zoom_input->Bind(wxEVT_COMBOBOX,[this](wxCommandEvent&){execute(Command::Zoom);});
        frame->Bind(wxEVT_CHAR_HOOK,[this](wxKeyEvent& e){
            ShortcutKey key=ShortcutKey::Other;const auto code=e.GetKeyCode();
            if(code==WXK_RETURN||code==WXK_NUMPAD_ENTER)key=ShortcutKey::Enter;
            else if(code==',')key=ShortcutKey::Comma;
            else if(code=='G'||code=='g')key=ShortcutKey::G;else if(code==WXK_F6)key=ShortcutKey::F6;else if(code==WXK_ESCAPE)key=ShortcutKey::Escape;
            bool extra_modifier=e.AltDown();
#ifdef __WXMAC__
            extra_modifier|=e.RawControlDown();
#endif
            if(!handle_shortcut(key,{false,input_composing(wxWindow::FindFocus()),e.CmdDown(),e.ShiftDown(),extra_modifier,false}))e.Skip();
        });
        frame->Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent&){close_window();});
        headings();frame->Show();frame->Layout();split->SetSashPosition(split->GetClientSize().x/2);
        frame->Bind(wxEVT_SIZING,[this](wxSizeEvent& e){capture_resize_anchor();e.Skip();});
        frame->Bind(wxEVT_SIZE,[this](wxSizeEvent& e){capture_resize_anchor();restore_after_resize();e.Skip();});
        split->Bind(wxEVT_SPLITTER_SASH_POS_CHANGING,[this](wxSplitterEvent& e){capture_resize_anchor();e.Skip();});
        split->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED,[this](wxSplitterEvent& e){restore_after_resize();e.Skip();});
        native_shortcuts=std::make_unique<NativeShortcutMonitor>(*frame,[this](ShortcutKey key,ShortcutContext state){return handle_shortcut(key,state);});
        stream_timer.Start(50);
        position_timer.Start(1000);
        io=std::make_unique<IoService>([this](auto event){if(io_inbox.push(std::move(event)))wxQueueEvent(&owner,new wxThreadEvent(IO_WAKE));});
        owner.CallAfter([this,initial]{
            if(closing)return;
            load_startup_model();
            // A password-protected initial PDF must not open another modal
            // while the missing-model picker is still active.
            if(!closing&&!initial.empty())open_path(initial);
        });
    }
};
ApplicationController::ApplicationController():impl_(std::make_unique<Impl>(*this)){}
ApplicationController::~ApplicationController()=default;
void ApplicationController::start(const wxString& initial){impl_->start(initial);}
}
