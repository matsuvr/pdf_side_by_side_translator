#include "app/controller.hpp"
#include "pdf/pdf_service.hpp"
#include "platform/font.hpp"
#include "platform/text_view.hpp"
#include "inference/inference_service.hpp"
#include "translation/page_attempt.hpp"
#include "translation/batch_run.hpp"
#include "platform/digest.hpp"
#include "storage/io_service.hpp"
#include "ui/pdf_canvas.hpp"
#include "ui/wx_path.hpp"
#include "product.hpp"
#include <wx/wx.h>
#include <wx/combobox.h>
#include <wx/display.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/splitter.h>
#include <wx/stdpaths.h>
#include <wx/textdlg.h>
#include <wx/wrapsizer.h>
#include <wx/clipbrd.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <limits>
#include <chrono>

namespace opt {
namespace {
wxDEFINE_EVENT(PDF_WAKE,wxThreadEvent);
wxDEFINE_EVENT(INFERENCE_WAKE,wxThreadEvent);
wxDEFINE_EVENT(IO_WAKE,wxThreadEvent);
enum class Command { Open, Close, Previous, Next, GoPage, FitWidth, FitPage, Zoom, Left, Right, Equal, Copy, SelectAll, CopySource, EditSource, Find, FindNext, FindPrevious, CloseSearch, Translate, Retranslate, TranslateAll, Stop, Automatic, Plain, Polite, ModelSettings, CpuRetry, ExportPage, ExportComplete, ClearCache, Help, About, Quit };
enum Id { Open=wxID_HIGHEST+400,Close,Previous,Next,GoPage,FitWidth,FitPage,Zoom,Left,Right,Equal,Help,About,CopySource,EditSource,Find,FindNext,FindPrevious,CloseSearch,Translate,Retranslate,TranslateAll,Stop,Automatic,Plain,Polite,ModelSettings,CpuRetry,ExportPage,ExportComplete,ClearCache };
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
class InferenceInbox {
    std::mutex mutex_;
    std::deque<std::shared_ptr<const InferenceEvent>> queue_;
    bool wake_=false;
public:
    bool push(std::shared_ptr<const InferenceEvent> event){
        std::lock_guard lock(mutex_);
        if(event->kind==InferenceEvent::Kind::TextDelta&&!queue_.empty()&&queue_.back()->kind==event->kind&&queue_.back()->stamp==event->stamp){
            if(queue_.back()->delta.size()+event->delta.size()<=4U*1024*1024){
                auto merged=std::make_shared<InferenceEvent>(*queue_.back());merged->delta+=event->delta;queue_.back()=std::move(merged);
            }
        }else{
            while(queue_.size()>=64)queue_.pop_front();
            queue_.push_back(std::move(event));
        }
        const bool notify=!wake_;wake_=true;return notify;
    }
    auto take(){std::lock_guard lock(mutex_);auto result=std::move(queue_);queue_.clear();wake_=false;return result;}
};
class IoInbox {
    std::mutex mutex_;std::deque<std::shared_ptr<const IoEvent>> queue_;bool wake_=false;
public:
    bool push(std::shared_ptr<const IoEvent> e){std::lock_guard lock(mutex_);while(queue_.size()>=64)queue_.pop_front();queue_.push_back(std::move(e));const bool notify=!wake_;wake_=true;return notify;}
    auto take(){std::lock_guard lock(mutex_);auto result=std::move(queue_);queue_.clear();wake_=false;return result;}
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
    wxTimer search_timer;
    wxTimer translate_timer,stream_timer;
    wxTimer position_timer,batch_timer;
    wxWindow *search_bar=nullptr,*left_container=nullptr;
    wxTextCtrl* search_input=nullptr;
    wxCheckBox* search_case=nullptr;
    wxStaticText* search_label=nullptr;
    struct SearchState {
        std::uint64_t generation=0;
        std::string query;
        bool active=false,backward=false;
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
    std::string document_key,native_document_key,model_key;
    std::uint32_t model_capacity=8192;
    ModelProfile model_profile;
    CacheContext attempt_context;
    struct PendingAttempt {std::uint64_t request;WorkStamp stamp;CacheContext context;std::vector<AttemptBlock> sources;bool view_only=false;};
    std::optional<PendingAttempt> pending_attempt;
    std::uint64_t next_io_request=0;
    bool document_state_loaded=false,reading_touched=false,page_position_touched=false,restoring_reading=false,storage_warning=false;
    bool preferences_loaded=false,preferences_touched=false;
    std::set<PageIndex> locally_edited;
    std::set<PageIndex> loaded_sources,requested_sources;
    std::vector<std::string> recent_paths;
    wxMenu* recent_menu=nullptr;
    std::optional<ReadingPosition> last_saved_position;
    std::optional<std::pair<PageIndex,std::size_t>> pending_text_anchor;
    ModelState model_state=ModelState::Unconfigured;
    ModelEpoch model_epoch=0;
    SettingsRevision settings_revision=1;
    std::filesystem::path model_path;
    BackendPreference backend_preference=BackendPreference::Automatic;
    Form form=Form::Plain;
    std::string backend,model_error,shown_translation;
    bool auto_translate=true,translation_dirty=false,translation_requested=false,force_requested=false;
    std::optional<std::tuple<DocumentEpoch,PageIndex,SourceRevision>> auto_suppressed;
    std::unique_ptr<PageAttempt> attempt;
    std::unique_ptr<PageAttempt> visible_attempt;
    CacheContext visible_context;
    std::unique_ptr<BatchRun> batch;
    bool batch_when_ready=false,attempt_view_only=false,explicit_page_request=false;
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
    std::map<PageIndex,std::shared_ptr<const std::string>> overrides;
    std::map<PageIndex,SourceRevision> source_revisions;
    std::optional<std::pair<PageIndex,Command>> pending_source_command;
    bool closing=false;

    explicit Impl(ApplicationController& receiver):owner(receiver),close_timer(&receiver),search_timer(&receiver),translate_timer(&receiver),stream_timer(&receiver),position_timer(&receiver),batch_timer(&receiver){
        owner.Bind(PDF_WAKE,[this](wxThreadEvent&){for(auto& e:inbox.take())if(!closing)receive(*e);});
        owner.Bind(INFERENCE_WAKE,[this](wxThreadEvent&){for(auto& e:inference_inbox.take())if(!closing)receive(*e);});
        owner.Bind(IO_WAKE,[this](wxThreadEvent&){for(auto& e:io_inbox.take())if(!closing)receive(*e);});
        owner.Bind(wxEVT_TIMER,[this](wxTimerEvent&){
            if(closing&&pdf->done()&&inference->done()&&(!io||io->done())){
                close_timer.Stop();pdf->join();inference->join();if(io)io->join();
                auto* window=frame;frame=nullptr;window->Destroy();
            }
        },close_timer.GetId());
        owner.Bind(wxEVT_TIMER,[this](wxTimerEvent&){begin_search(false,false);},search_timer.GetId());
        owner.Bind(wxEVT_TIMER,[this](wxTimerEvent&){begin_translation();},translate_timer.GetId());
        owner.Bind(wxEVT_TIMER,[this](wxTimerEvent&){if(translation_dirty)refresh_translation();},stream_timer.GetId());
        owner.Bind(wxEVT_TIMER,[this](wxTimerEvent&){save_reading_position();},position_timer.GetId());
        owner.Bind(wxEVT_TIMER,[this](wxTimerEvent&){advance_batch();},batch_timer.GetId());
        pdf=std::make_unique<PdfService>([this](auto event){
            if(event->document!=wanted_epoch.load())return;
            if(inbox.push(std::move(event)))wxQueueEvent(&owner,new wxThreadEvent(PDF_WAKE));
        },native_path(wxStandardPaths::Get().GetResourcesDir()));
        inference=std::make_unique<InferenceService>([this](auto event){
            if(event->model!=wanted_model.load())return;
            if(inference_inbox.push(std::move(event)))wxQueueEvent(&owner,new wxThreadEvent(INFERENCE_WAKE));
        });
    }
    ~Impl(){pdf->stop();inference->stop();if(io)io->stop();pdf->join();inference->join();if(io)io->join();}
    bool can(Command command) const {
        if(closing)return false;
        switch(command){
        case Command::Open:return !pdf->done();
        case Command::Translate:case Command::Retranslate:case Command::TranslateAll:return document_state==DocumentState::Ready&&model_state!=ModelState::Loading;
        case Command::Stop:return (attempt&&attempt->state()==PageState::Running)||translation_requested||pending_attempt.has_value()||(batch&&batch->active());
        case Command::CpuRetry:return !model_path.empty()&&model_state!=ModelState::Loading;
        case Command::ExportPage:{const auto* shown=current_attempt();return shown&&std::any_of(shown->snapshot().chunks.begin(),shown->snapshot().chunks.end(),[](const auto& c){return !c.text.empty();});}
        case Command::ExportComplete:return document_state==DocumentState::Ready&&io&&model_state==ModelState::Ready;
        case Command::Close:return document_state!=DocumentState::Empty;
        case Command::Previous:return document_state==DocumentState::Ready&&page>0;
        case Command::Next:return document_state==DocumentState::Ready&&page+1<page_count;
        case Command::GoPage:case Command::FitWidth:case Command::FitPage:case Command::Zoom:
        case Command::Left:case Command::Right:case Command::CopySource:case Command::EditSource:
        case Command::Find:case Command::FindNext:case Command::FindPrevious:return document_state==DocumentState::Ready;
        case Command::Copy:
            if(wxWindow::FindFocus()==canvas)return canvas->has_selection();
            if(auto* text=wxDynamicCast(wxWindow::FindFocus(),wxTextCtrl))return text->CanCopy();
            return false;
        default:return true;
        }
    }
    auto current_source_identity()const{return std::tuple{epoch,page,source_revisions.contains(page)?source_revisions.at(page):SourceRevision{1}};}
    const PageAttempt* current_attempt()const{
        if(attempt&&attempt_context.page==page)return attempt.get();
        if(visible_attempt&&visible_context.page==page)return visible_attempt.get();return nullptr;
    }
    void show_translation(std::string text,bool reset=true){
        if(text==shown_translation&&!reset)return;
        update_readonly_text(*translation,shown_translation,text,reset);shown_translation=std::move(text);
    }
    void invalidate_translation(){
        translate_timer.Stop();translation_requested=false;force_requested=false;inference->cancel_job();
        pending_attempt.reset();scheduled_page.reset();explicit_page_request=false;
        if(attempt){attempt->interrupt();save_attempt();attempt.reset();}visible_attempt.reset();attempt_view_only=false;
        translation_dirty=false;
    }
    void storage_result(bool accepted){if(!accepted){storage_warning=true;frame->SetStatusText(wxS("保存待ちの件数が上限に達しました。この変更は未保存です。"),0);}}
    void save_attempt(){if(io&&attempt)storage_result(io->save_attempt(attempt_context,attempt->snapshot()));}
    void request_source(PageIndex target){
        if(loaded_sources.contains(target)||requested_sources.contains(target))return;
        if(!io||native_document_key.empty()){loaded_sources.insert(target);return;}
        if(io->done()){loaded_sources.insert(target);storage_warning=true;return;}
        if(io->read_page(epoch,document_key,native_document_key,target))requested_sources.insert(target);else{storage_result(false);if(scheduled_page&&*scheduled_page==target)translate_timer.StartOnce(150);}
    }
    void save_reading_position(){
        if(!io||!document_state_loaded||restoring_reading||document_state!=DocumentState::Ready)return;
        ReadingPosition position;position.page=page;position.pdf_anchor=canvas->reading_anchor();
        position.text_anchor=pending_text_anchor&&pending_text_anchor->first==page?pending_text_anchor->second:text_logical_anchor(*translation);
        if(last_saved_position==position)return;last_saved_position=position;
        storage_result(io->save_position(document_key,path,position));
    }
    void save_preferences(){
        if(!io||!preferences_loaded)return;
        const auto bytes=model_path.u8string();
        std::map<std::string,std::string> values{{"model-path",std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size())},
            {"backend",backend_preference==BackendPreference::Cpu?"cpu":"auto"},{"form",form==Form::Plain?"plain":"polite"},{"auto-translate",auto_translate?"1":"0"},
            {"zoom-mode",std::to_string(static_cast<unsigned>(canvas->zoom_mode()))},{"zoom-percent",std::to_string(canvas->zoom_percent())}};
        if(split->GetClientSize().x>0)values["split-ratio"]=std::to_string(static_cast<double>(split->GetSashPosition())/split->GetClientSize().x);
        values["window-maximized"]=frame->IsMaximized()?"1":"0";
        if(!frame->IsMaximized()&&!frame->IsIconized()){
            const auto size=frame->ToDIP(frame->GetSize());const auto position=frame->GetPosition();
            values["window-width"]=std::to_string(size.x);values["window-height"]=std::to_string(size.y);
            values["window-x"]=std::to_string(position.x);values["window-y"]=std::to_string(position.y);
        }
        storage_result(io->settings(std::move(values)));
    }
    void refresh_translation(){
        translation_dirty=false;const auto* shown=current_attempt();if(!shown)return;
        auto text=shown->display_text();wxString status;
        switch(shown->state()){
        case PageState::Complete:status=wxS("完了");break;
        case PageState::Partial:status=wxS("部分結果・再試行できます");break;
        case PageState::Failed:status=wxS("失敗・原文を確認して再試行できます");break;
        case PageState::NoText:
            status=wxS("翻訳するテキストがありません");
            if(text.empty())text="翻訳するテキストがありません。「翻訳用原文の確認・編集」から文章を入力できます。";
            if(!overrides.contains(page))if(auto layout=current_layout();layout&&layout->availability==PageLayout::Availability::UnicodeUnavailable){status=wxS("文字のUnicode情報を取得できません");text="文字のUnicode情報を取得できません。原文画像を確認し、「翻訳用原文の確認・編集」から文章を入力できます。";}
            break;
        default:status=wxS("翻訳中");if(text.empty())text="翻訳しています…";break;
        }
        show_translation(std::move(text),false);
        if(pending_text_anchor&&pending_text_anchor->first==page&&(shown->state()==PageState::Complete||shown->state()==PageState::Partial||shown->state()==PageState::Failed||shown->state()==PageState::NoText)){
            restore_text_logical_anchor(*translation,pending_text_anchor->second);pending_text_anchor.reset();
        }
        right_heading->SetLabel(wxString::Format(wxS("訳文 — %uページ・"),page+1)+status);right_heading->GetParent()->Layout();
        if(!batch)frame->SetStatusText(status,2);else batch_status();
    }
    void schedule_translation(bool explicit_request=false,bool force=false){
        if(explicit_request){translation_requested=true;force_requested=force;explicit_page_request=true;auto_suppressed.reset();}
        if(document_state!=DocumentState::Ready||model_state!=ModelState::Ready)return;
        if(!translation_requested&&!(batch&&batch->active())&&(!auto_translate||(auto_suppressed&&*auto_suppressed==current_source_identity())))return;
        scheduled_page=page;
        translate_timer.StartOnce(explicit_request?1:150);
    }
    void batch_status(){
        if(!batch)return;const auto& c=batch->counts();
        frame->SetStatusText(wxString::Format(wxS("完了 %u/%u・文字なし %u・部分 %u・失敗 %u"),c.complete,batch->total(),c.no_text,c.partial,c.failed),2);
    }
    void stop_batch(bool forget=false){if(batch)batch->stop();if(forget)batch.reset();batch_timer.Stop();batch_when_ready=false;}
    void start_batch(){
        batch=std::make_unique<BatchRun>(page_count,profile_digest(model_profile,form,model_capacity)+":"+std::to_string(model_epoch));auto_suppressed.reset();
        if(attempt&&attempt_context.page==page){pump_translation();}else schedule_translation(true);
        batch_status();
    }
    void advance_batch(){
        if(!batch||!batch->active()||closing||pending_attempt||(attempt&&attempt->state()==PageState::Running))return;
        auto next=batch->next(page);if(!next){batch_status();return;}
        scheduled_page=*next;translation_requested=true;explicit_page_request=false;translate_timer.StartOnce(1);
    }
    void pump_translation(){
        if(!attempt||model_state!=ModelState::Ready)return;
        if(!attempt_view_only)if(auto request=attempt->next_request()){
            if(!inference->plan(*request))attempt->preparation_failed(request->stamp,FinishReason::ModelUnavailable);
        }
        if(batch&&batch->active()&&attempt->state()!=PageState::Running&&attempt->state()!=PageState::Queued){
            batch->terminal(attempt_context.page,attempt->state());batch_status();if(batch->active())batch_timer.StartOnce(1);
        }
        translation_dirty=true;
    }
    void begin_translation(){
        if(closing||document_state!=DocumentState::Ready||model_state!=ModelState::Ready)return;
        if(!translation_requested&&!(batch&&batch->active())&&(!auto_translate||(auto_suppressed&&*auto_suppressed==current_source_identity())))return;
        const auto target=scheduled_page.value_or(page);
        auto found=layouts.find(target);auto layout=found==layouts.end()?nullptr:found->second.layout;
        if(!layout){request_layout(target);return;}
        if(!loaded_sources.contains(target)){request_source(target);return;}
        const bool force=force_requested;force_requested=false;translation_requested=false;
        const bool view_only=batch&&batch->contains(target)&&!explicit_page_request;explicit_page_request=false;scheduled_page.reset();
        if(pending_attempt&&!force)return;
        if(attempt&&attempt_context.page==target&&!force){
            if(attempt->state()==PageState::Running)return;
            if(attempt->state()!=PageState::Complete&&attempt->state()!=PageState::NoText&&!view_only)attempt->resume();attempt_view_only=view_only;pump_translation();refresh_translation();return;
        }
        if(attempt){
            attempt->interrupt();save_attempt();inference->cancel_job();
            if(attempt_context.page==page&&target!=page){visible_attempt=std::move(attempt);visible_context=attempt_context;}else attempt.reset();
        }
        const auto edited=overrides.find(target);
        auto sources=PageAttempt::sources(*layout,edited==overrides.end()?nullptr:edited->second);
        CacheContext context{document_key,model_key,profile_digest(model_profile,form,model_capacity),
            page_source_digest(*layout,edited==overrides.end()?nullptr:edited->second),target};
        if(edited!=overrides.end())context.override_digest=sha256(*edited->second);
        WorkStamp stamp;stamp.document=epoch;stamp.page=target;stamp.model=model_epoch;stamp.source=source_revisions.contains(target)?source_revisions.at(target):1;stamp.settings=settings_revision;
        attempt.reset();
        if(!force&&io){
            pending_attempt=PendingAttempt{++next_io_request,stamp,context,std::move(sources),view_only};
            if(io->read_attempt(context,pending_attempt->request,epoch)){if(!batch)frame->SetStatusText(wxS("保存済みの訳文を確認しています…"),2);return;}
            sources=std::move(pending_attempt->sources);pending_attempt.reset();storage_result(false);
        }
        attempt_context=context;
        attempt_view_only=view_only;attempt=std::make_unique<PageAttempt>(stamp,context.condition_key(),std::move(sources),form,force);
        save_attempt();pump_translation();refresh_translation();
    }
    void load_model(bool restoring=false){
        if(!restoring)preferences_touched=true;
        if(inference->done()){model_state=ModelState::Failed;frame->SetStatusText(wxS("推論処理が終了しました。アプリを再起動してください。"),1);return;}
        const bool requested=translation_requested,force=force_requested;
        invalidate_translation();translation_requested=requested;force_requested=force;
        stop_batch(true);model_state=ModelState::Loading;model_error.clear();backend.clear();wanted_model=++model_epoch;
        model_key="provisional:model:"+new_uuid();save_preferences();
        frame->SetStatusText(wxS("モデル読込中…"),1);inference->load(model_epoch,model_path,backend_preference);
    }
    void model_settings(){
        wxDialog dialog(frame,wxID_ANY,wxS("モデル設定"),wxDefaultPosition,frame->FromDIP(wxSize(720,300)),wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto root=new wxBoxSizer(wxVERTICAL);const auto gap=frame->FromDIP(8);
        auto description=new wxStaticText(&dialog,wxID_ANY,wxS("PLaMo 3 Translate 0.8B のローカルGGUFを選択してください。\nモデルは同梱・自動ダウンロードされません。利用条件はモデルの配布元で確認してください。"));
        root->Add(description,0,wxALL|wxEXPAND,gap);
        if(!model_error.empty()){
            auto failure=new wxStaticText(&dialog,wxID_ANY,wxString::FromUTF8(model_error));failure->Wrap(frame->FromDIP(660));root->Add(failure,0,wxALL|wxEXPAND,gap);
        }
        auto row=new wxBoxSizer(wxHORIZONTAL);auto path_field=new wxTextCtrl(&dialog,wxID_ANY,display_path(model_path));path_field->SetName(wxS("GGUFファイル"));row->Add(path_field,1,wxALL|wxEXPAND,gap);
        auto browse=new wxButton(&dialog,wxID_ANY,wxS("ファイルを選択…"));row->Add(browse,0,wxALL,gap);root->Add(row,0,wxEXPAND);
        browse->Bind(wxEVT_BUTTON,[&](wxCommandEvent&){wxFileDialog file(&dialog,wxS("GGUFを選択"),{}, {},wxS("GGUF (*.gguf)|*.gguf|すべてのファイル|*"),wxFD_OPEN|wxFD_FILE_MUST_EXIST);if(file.ShowModal()==wxID_OK)path_field->ChangeValue(file.GetPath());});
        wxArrayString choices;choices.Add(wxS("自動（Apple SiliconではMetal、利用できなければCPU）"));choices.Add(wxS("CPU"));
        auto preference=new wxChoice(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,choices);preference->SetSelection(backend_preference==BackendPreference::Cpu?1:0);preference->SetName(wxS("推論デバイス"));root->Add(preference,0,wxALL|wxEXPAND,gap);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxALL|wxALIGN_RIGHT,gap);dialog.SetSizer(root);dialog.CentreOnParent();
        if(dialog.ShowModal()!=wxID_OK||path_field->GetValue().empty())return;
        model_path=native_path(path_field->GetValue());backend_preference=preference->GetSelection()==1?BackendPreference::Cpu:BackendPreference::Automatic;load_model();
    }
    void receive(const InferenceEvent& event){
        if(event.model!=model_epoch)return;
        switch(event.kind){
        case InferenceEvent::Kind::ModelReady:
            model_state=ModelState::Ready;backend=event.backend;model_capacity=event.context_capacity;model_profile=event.profile;
            if(event.file)model_key="provisional:model:"+provisional_file_key(event.file->initial_signature());
            if(io&&event.file)storage_result(io->track_model(model_epoch,model_key,model_path,event.file));
            frame->SetStatusText(wxS("モデル準備完了・")+wxString::FromUTF8(backend),1);
            if(batch_when_ready){batch_when_ready=false;start_batch();}else schedule_translation();break;
        case InferenceEvent::Kind::ModelFailed:
            model_state=ModelState::Failed;model_error=event.detail;
            frame->SetStatusText(wxString::FromUTF8(event.message),1);
            if(attempt){attempt->interrupt();translation_dirty=true;}
            break;
        case InferenceEvent::Kind::Prepared:
            if(attempt&&attempt->prepared(event.chunk)){
                if(auto prepared=attempt->active_prepared()){
                    if(!attempt->may_read_cache()||!io||!io->lookup(attempt_context,prepared)){
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
                save_attempt();
                if(event.result.reason==FinishReason::ModelUnavailable){model_state=ModelState::Failed;model_error=event.detail;stop_batch(true);frame->SetStatusText(wxS("モデルを利用できません。モデル設定から読み直してください。"),1);}
                else pump_translation();refresh_translation();
            }break;
        default:break;
        }
    }
    void promote_document_key(const std::string& from,const std::string& key){
        if(document_key!=from)return;document_key=key;
        if(attempt){attempt_context.document_key=key;attempt->promote_condition_key(attempt_context.condition_key());}
        if(visible_attempt){visible_context.document_key=key;visible_attempt->promote_condition_key(visible_context.condition_key());}
        if(pending_attempt)pending_attempt->context.document_key=key;
    }
    void receive(const IoEvent& event){
        switch(event.kind){
        case IoEvent::Kind::Initialized:{
            storage_warning=!event.persistent;recent_paths=event.recent_paths;
            for(std::size_t i=0;i<recent_paths.size();++i){
                const int id=wxID_HIGHEST+800+static_cast<int>(i);recent_menu->Append(id,wxString::FromUTF8(recent_paths[i]));
                frame->Bind(wxEVT_MENU,[this,i](wxCommandEvent&){if(i<recent_paths.size())open_path(wxString::FromUTF8(recent_paths[i]));},id);
            }
            const auto& values=event.settings;
            preferences_loaded=true;
            if(preferences_touched){save_preferences();break;}
            auto value=[&](const char* key)->std::string{auto at=values.find(key);return at==values.end()?std::string{}:at->second;};
            auto_translate=value("auto-translate")!="0";form=value("form")=="polite"?Form::Polite:Form::Plain;
            backend_preference=value("backend")=="cpu"?BackendPreference::Cpu:BackendPreference::Automatic;
            model_path=native_path(wxString::FromUTF8(value("model-path")));
            double number=0;
            if(wxString::FromUTF8(value("split-ratio")).ToDouble(&number)&&std::isfinite(number)&&number>=0.15&&number<=0.85)split->SetSashPosition(static_cast<int>(split->GetClientSize().x*number));
            long mode=0;double percent=100;
            if(wxString::FromUTF8(value("zoom-mode")).ToLong(&mode)&&mode>=0&&mode<=2&&wxString::FromUTF8(value("zoom-percent")).ToDouble(&percent)&&std::isfinite(percent)&&percent>0){
                canvas->set_zoom(static_cast<ZoomMode>(mode),percent);
                zoom_input->ChangeValue(mode==0?wxS("幅に合わせる"):mode==1?wxS("ページ全体"):wxString::Format("%.1f%%",percent));
            }
            long width=0,height=0,x=0,y=0;
            if(wxString::FromUTF8(value("window-width")).ToLong(&width)&&wxString::FromUTF8(value("window-height")).ToLong(&height)&&width>=640&&height>=400&&width<=10000&&height<=10000){
                auto size=frame->FromDIP(wxSize(static_cast<int>(width),static_cast<int>(height)));
                bool position_ok=wxString::FromUTF8(value("window-x")).ToLong(&x)&&wxString::FromUTF8(value("window-y")).ToLong(&y)&&x>=-100000&&x<=100000&&y>=-100000&&y<=100000;
                const auto display=position_ok?wxDisplay::GetFromPoint(wxPoint(static_cast<int>(x),static_cast<int>(y))):wxNOT_FOUND;
                const auto area=wxDisplay(display==wxNOT_FOUND?0U:static_cast<unsigned>(display)).GetClientArea();
                size.x=std::min(size.x,area.width);size.y=std::min(size.y,area.height);
                frame->SetSize(wxRect(wxPoint(std::clamp(static_cast<int>(x),area.x,area.GetRight()-size.x+1),std::clamp(static_cast<int>(y),area.y,area.GetBottom()-size.y+1)),size));
                if(value("window-maximized")=="1")frame->Maximize();
            }
            if(!event.persistent)frame->SetStatusText(wxS("保存先を利用できません。変更はこの起動中だけ保持されます。"),0);
            else if(!event.warnings.empty())frame->SetStatusText(wxS("保存データの一部を読み込めません。現在の操作は続けられます。"),0);
            if(!model_path.empty())load_model(true);break;
        }
        case IoEvent::Kind::DocumentLoaded:
            if(event.document!=epoch||!event.saved_document)break;
            promote_document_key(event.from_key,event.key);document_state_loaded=true;
            loaded_sources.insert(event.saved_document->position.page);
            for(const auto& v:event.saved_document->overrides)if(!locally_edited.contains(v.page)&&v.page<page_count){
                source_revisions[v.page]=std::max(SourceRevision{1},v.revision);
                if(v.source)overrides[v.page]=std::make_shared<const std::string>(*v.source);else overrides.erase(v.page);
            }
            if(!reading_touched){
                const auto& position=event.saved_document->position;
                if(position.page<page_count){restoring_reading=true;go(position.page);restoring_reading=false;}
                if(position.pdf_anchor){anchors[page]=*position.pdf_anchor;canvas->restore_anchor(*position.pdf_anchor);}
                pending_text_anchor=std::pair{page,position.text_anchor};
            }
            if(attempt||pending_attempt)invalidate_translation();schedule_translation();break;
        case IoEvent::Kind::PageLoaded:{
            if(event.document!=epoch||!event.saved_document)break;
            const auto target=event.saved_document->position.page;requested_sources.erase(target);loaded_sources.insert(target);
            if(target==page&&!page_position_touched){
                if(event.saved_document->position.pdf_anchor)canvas->restore_anchor(*event.saved_document->position.pdf_anchor);
                pending_text_anchor=std::pair{target,event.saved_document->position.text_anchor};
            }
            if(!locally_edited.contains(target)){
                overrides.erase(target);source_revisions[target]=1;
                for(const auto& v:event.saved_document->overrides){source_revisions[target]=std::max(SourceRevision{1},v.revision);if(v.source)overrides[target]=std::make_shared<const std::string>(*v.source);}
            }
            for(auto i=loaded_sources.begin();loaded_sources.size()>2&&i!=loaded_sources.end();){
                if(*i!=page&&(!attempt||*i!=attempt_context.page)&&*i!=target){overrides.erase(*i);locally_edited.erase(*i);i=loaded_sources.erase(i);}else ++i;
            }
            if(target==page){
                if(pending_source_command&&pending_source_command->first==page){const auto command=pending_source_command->second;pending_source_command.reset();execute(command);}
                schedule_translation();
            }
            if(scheduled_page&&*scheduled_page==target)translate_timer.StartOnce(1);
            if(!event.detail.empty()){storage_warning=true;frame->SetStatusText(wxS("このページの保存済み原文を読み込めませんでした。"),0);}break;
        }
        case IoEvent::Kind::Fingerprint:
            if(event.model_fingerprint){
                if(event.model!=model_epoch||event.from_key!=model_key)break;model_key=event.key;
                if(attempt){attempt_context.model_key=event.key;attempt->promote_condition_key(attempt_context.condition_key());}
                if(visible_attempt){visible_context.model_key=event.key;visible_attempt->promote_condition_key(visible_context.condition_key());}
                if(pending_attempt)pending_attempt->context.model_key=event.key;
            }else if(event.document==epoch)promote_document_key(event.from_key,event.key);
            break;
        case IoEvent::Kind::AttemptLoaded:
            if(event.document!=epoch||!pending_attempt||pending_attempt->request!=event.request)break;
            attempt_context=pending_attempt->context;
            attempt_view_only=pending_attempt->view_only;
            attempt=std::make_unique<PageAttempt>(pending_attempt->stamp,attempt_context.condition_key(),std::move(pending_attempt->sources),form,false,event.attempt,!attempt_view_only);
            pending_attempt.reset();save_attempt();pump_translation();refresh_translation();break;
        case IoEvent::Kind::CacheResult:
            if(!attempt||!attempt->accepts(event.stamp))break;
            if(event.cached_text){attempt->finished(event.stamp,{FinishReason::Completed,*event.cached_text,0});save_attempt();pump_translation();refresh_translation();}
            else if(auto prepared=attempt->active_prepared()){
                if(!inference->execute(prepared)){attempt->preparation_failed(event.stamp,FinishReason::ModelUnavailable);save_attempt();pump_translation();}
            }break;
        case IoEvent::Kind::OverrideSaved:
            if(event.document==epoch)frame->SetStatusText(event.persistent?wxS("翻訳用原文を保存しました。"):wxS("翻訳用原文はこのセッション内だけで保持されています。"),0);
            if(!event.persistent)storage_warning=true;break;
        case IoEvent::Kind::Exported:frame->SetStatusText(wxString::FromUTF8(event.message),0);break;
        case IoEvent::Kind::Error:storage_warning=true;frame->SetStatusText(wxString::FromUTF8(event.message),0);break;
        default:break;
        }
    }
    void cancel_search(bool clear_hit=true){
        ++search.generation;search.active=false;search_timer.Stop();
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
        ++search.generation;search.query=query;search.backward=backward;search.cursor=page;
        search.start=resume?(backward?search.hit->second.begin:search.hit->second.end):
            backward?std::numeric_limits<std::size_t>::max():0;
        search.remaining=page_count+(resume?1U:0U);search.active=!query.empty();
        if(!resume){search.hit.reset();canvas->show_search_hit({});}
        if(search.active)search_step();else search_label->SetLabel(wxString{});
    }
    void show_search(){
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
        std::size_t bytes=sizeof(PageLayout)+layout->glyphs.size()*sizeof(GlyphRecord)+layout->reading_text_utf8.size()+layout->lines.size()*sizeof(TextLine);
        auto mappings=[&](const auto& values){bytes+=values.size()*sizeof(TextMapping);for(const auto& m:values)bytes+=m.source_ranges.size()*sizeof(SourceCharRange);};
        mappings(layout->reading_mapping);
        for(const auto& block:layout->blocks){
            bytes+=sizeof(TextBlock)+block.reading_text_utf8.size()+block.translation_input_utf8.size()+block.source_rects.size()*sizeof(PdfRect)+block.lines.size()*sizeof(TextLine);
            mappings(block.reading_mapping);mappings(block.translation_mapping);
        }
        if(auto old=layouts.find(index);old!=layouts.end())layout_bytes-=old->second.bytes;
        layouts.insert_or_assign(index,CachedLayout{std::move(layout),bytes,++layout_tick});layout_bytes+=bytes;
        while(layout_bytes>32U*1024*1024&&layouts.size()>1){
            auto victim=layouts.end();
            for(auto i=layouts.begin();i!=layouts.end();++i)if(i->first!=page&&(victim==layouts.end()||i->second.used<victim->second.used))victim=i;
            if(victim==layouts.end())break;layout_bytes-=victim->second.bytes;layouts.erase(victim);
        }
    }
    void edit_source(const PageLayout& layout){
        const auto editing_page=page;const auto editing_document=epoch;
        cancel_search();
        wxDialog dialog(frame,wxID_ANY,wxS("翻訳用原文の確認・編集"),wxDefaultPosition,frame->FromDIP(wxSize(820,640)),wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto root=new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog,wxID_ANY,wxS("このページの翻訳に使う文章です。PDFの原本、原文のコピー、検索は変わりません。")),0,wxALL,frame->FromDIP(8));
        const auto existing=overrides.find(page);
        const auto& source=existing==overrides.end()?layout.reading_text_utf8:*existing->second;
        auto text=new wxTextCtrl(&dialog,wxID_ANY,wxString::FromUTF8(source),wxDefaultPosition,wxDefaultSize,wxTE_MULTILINE);
        text->SetName(wxS("翻訳用原文"));text->SetFont(translation->GetFont());root->Add(text,1,wxEXPAND|wxALL,frame->FromDIP(8));
        auto buttons=new wxBoxSizer(wxHORIZONTAL);
        auto reset=new wxButton(&dialog,wxID_REVERT_TO_SAVED,wxS("自動抽出へ戻す"));buttons->Add(reset,0,wxALL,frame->FromDIP(4));buttons->AddStretchSpacer();
        buttons->Add(new wxButton(&dialog,wxID_CANCEL,wxS("キャンセル")),0,wxALL,frame->FromDIP(4));
        auto use=new wxButton(&dialog,wxID_OK,wxS("この原文を使う"));use->SetDefault();buttons->Add(use,0,wxALL,frame->FromDIP(4));root->Add(buttons,0,wxEXPAND|wxALL,frame->FromDIP(4));
        reset->Bind(wxEVT_BUTTON,[&](wxCommandEvent&){dialog.EndModal(wxID_REVERT_TO_SAVED);});
        dialog.SetSizer(root);dialog.CentreOnParent();text->SetFocus();
        const int choice=dialog.ShowModal();
        if(editing_document!=epoch)return;
        if(choice==wxID_OK)overrides[editing_page]=std::make_shared<const std::string>(text->GetValue().ToStdString(wxConvUTF8));
        else if(choice==wxID_REVERT_TO_SAVED)overrides.erase(editing_page);
        else return;
        auto& revision=source_revisions[editing_page];revision=std::max(revision+1,static_cast<SourceRevision>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
        locally_edited.insert(editing_page);
        if(io&&!native_document_key.empty()){
            SavedOverride value;value.page=editing_page;value.revision=revision;
            if(auto found=overrides.find(editing_page);found!=overrides.end())value.source=*found->second;
            storage_result(io->save_override(epoch,native_document_key,document_key,path,std::move(value)));
            frame->SetStatusText(wxS("翻訳用原文を更新しました。保存しています…"),0);
        }else frame->SetStatusText(wxS("翻訳用原文はこのセッション内だけで保持されています。"),0);
        stop_batch(true);if(editing_page==page){invalidate_translation();schedule_translation();}
    }
    void route(int id,Command command){
        frame->Bind(wxEVT_MENU,[this,command](wxCommandEvent&){execute(command);},id);
        frame->Bind(wxEVT_BUTTON,[this,command](wxCommandEvent&){execute(command);},id);
        frame->Bind(wxEVT_UPDATE_UI,[this,command](wxUpdateUIEvent& e){e.Enable(can(command));},id);
    }
    void headings(){
        left_heading->SetLabel(page_count?wxString::Format(wxS("原文 — %uページ"),page+1):wxS("原文"));
        right_heading->SetLabel(page_count?wxString::Format(wxS("訳文 — %uページ"),page+1):wxS("訳文"));
        page_input->ChangeValue(page_count?wxString::Format("%u",page+1):wxString{});
        page_count_label->SetLabel(wxString::Format("/ %u",page_count));
        frame->SetStatusText(page_count?wxString::Format(wxS("%u / %uページ"),page+1,page_count):wxS("PDFを開いてください。"),0);
        frame->Layout();
    }
    void open_path(const wxString& name){
        if(closing)return;
        save_reading_position();if(io)io->close_document();
        stop_batch(true);invalidate_translation();auto_suppressed.reset();
        document_key="provisional:document:"+new_uuid();native_document_key.clear();document_state_loaded=false;reading_touched=false;page_position_touched=false;pending_text_anchor.reset();last_saved_position.reset();locally_edited.clear();loaded_sources.clear();requested_sources.clear();
        wxFileName absolute(name);absolute.MakeAbsolute();path=native_path(absolute.GetFullPath());document_state=DocumentState::Opening;page=0;page_count=0;
        cancel_search();search_input->ChangeValue(wxString{});search.query.clear();search_label->SetLabel(wxString{});
        anchors.clear();layouts.clear();layout_bytes=0;requested_layouts.clear();overrides.clear();source_revisions.clear();pending_source_command.reset();wanted_epoch=++epoch;
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
        invalidate_translation();auto_suppressed.reset();
        if(!search_navigation)cancel_search();
        if(auto a=canvas->reading_anchor())anchors[page]=*a;
        page=target;canvas->set_page(epoch,page);show_translation(model_state==ModelState::Ready?"ページの原文を準備しています…":"翻訳モデルを設定すると、このページの訳文を表示します。");
        pending_source_command.reset();if(auto layout=current_layout())canvas->set_layout(std::move(layout));
        headings();pdf->page_info(epoch,page);request_source(page);schedule_translation();
    }
    void receive(const PdfEvent& event){
        if(event.document!=epoch)return;
        switch(event.kind){
        case PdfEvent::Kind::Opened:
            document_state=DocumentState::Ready;page_count=event.page_count;page=0;
            native_document_key=provisional_file_key(event.file_signature);
            document_key="provisional:document:"+native_document_key;
            if(io&&event.file_snapshot)storage_result(io->open_document(epoch,document_key,path,event.file_snapshot));
            canvas->set_page(epoch,0,event.info);headings();
            show_translation(model_state==ModelState::Ready?"ページの原文を準備しています…":"翻訳モデルを設定すると、このページの訳文を表示します。");schedule_translation();
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
                cache_layout(event.page,event.layout);source_revisions.try_emplace(event.page,1);
                if(event.page==page){
                    canvas->set_layout(event.layout);
                    if(event.layout->availability==PageLayout::Availability::NoTextLayer||event.layout->availability==PageLayout::Availability::NoTranslatableText)
                        show_translation("翻訳するテキストがありません。「翻訳用原文の確認・編集」から文章を入力できます。");
                    else if(event.layout->availability==PageLayout::Availability::UnicodeUnavailable)
                        show_translation("文字のUnicode情報を取得できません。原文画像を確認し、翻訳用原文を入力できます。");
                    if(pending_source_command&&pending_source_command->first==page){const auto command=pending_source_command->second;pending_source_command.reset();execute(command);}
                    schedule_translation();
                }
                if(scheduled_page&&*scheduled_page==event.page)translate_timer.StartOnce(1);
            }
            break;
        case PdfEvent::Kind::SearchResult:
            if(event.layout)cache_layout(event.page,event.layout);
            if(!search.active||event.query_generation!=search.generation||event.page!=search.cursor)break;
            if(event.hit){
                search.active=false;search.hit=std::pair{event.page,*event.hit};
                go(event.page,true);if(auto layout=current_layout())canvas->set_layout(layout);
                canvas->show_search_hit(event.hit);search_label->SetLabel(wxString::Format(wxS("%uページ"),event.page+1));
            }else if(search.remaining>1){
                --search.remaining;
                search.cursor=search.backward?(search.cursor==0?page_count-1:search.cursor-1):(search.cursor+1)%page_count;
                search.start=search.backward?std::numeric_limits<std::size_t>::max():0;search_step();
            }else{search.active=false;search.hit.reset();canvas->show_search_hit({});search_label->SetLabel(wxS("見つかりませんでした。"));}
            left_container->Layout();break;
        case PdfEvent::Kind::PasswordRequired:{
            wxPasswordEntryDialog dialog(frame,wxS("PDFのパスワードを入力してください。"),wxS("パスワード付きPDF"));
            if(dialog.ShowModal()==wxID_OK)pdf->open(epoch,path,dialog.GetValue().ToStdString(wxConvUTF8));
            else execute(Command::Close);
            break;
        }
        case PdfEvent::Kind::Failed:
            requested_layouts.erase(event.page);
            if(pending_source_command&&pending_source_command->first==event.page)pending_source_command.reset();
            if(event.code=="PdfSearch"&&event.query_generation==search.generation){search.active=false;search_label->SetLabel(wxS("検索できませんでした。PDFを再度開けます。"));}
            if(event.code=="PdfOpen"){document_state=DocumentState::Failed;show_translation(event.message);}
            if(event.code=="PdfService"){stop_batch(true);invalidate_translation();document_state=DocumentState::Failed;show_translation(event.message);}
            if(event.code=="PdfPage"&&scheduled_page&&*scheduled_page==event.page){
                scheduled_page.reset();translation_requested=false;
                if(batch&&batch->active()){batch->terminal(event.page,PageState::Failed);batch_status();if(batch->active())batch_timer.StartOnce(1);}
            }
            if(event.code=="PdfOpen"||event.page==page)frame->SetStatusText(wxString::FromUTF8(event.message),0);
            break;
        default:break;
        }
    }
    void execute(Command command){
        if(!can(command))return;
        switch(command){case Command::FitWidth:case Command::FitPage:case Command::Zoom:case Command::Automatic:case Command::Plain:case Command::Polite:preferences_touched=true;break;default:break;}
        switch(command){
        case Command::Open:{
            wxFileDialog dialog(frame,wxS("PDFを開く"),{}, {},wxS("PDFファイル (*.pdf)|*.pdf|すべてのファイル|*"),wxFD_OPEN|wxFD_FILE_MUST_EXIST);
            if(dialog.ShowModal()==wxID_OK)open_path(dialog.GetPath());break;
        }
        case Command::Close:
            save_reading_position();if(io)io->close_document();document_state_loaded=false;last_saved_position.reset();
            stop_batch(true);invalidate_translation();auto_suppressed.reset();
            loaded_sources.clear();requested_sources.clear();locally_edited.clear();
            cancel_search();search_input->ChangeValue(wxString{});search.query.clear();search_label->SetLabel(wxString{});
            wanted_epoch=++epoch;pdf->close(epoch);document_state=DocumentState::Empty;page_count=0;page=0;
            anchors.clear();layouts.clear();layout_bytes=0;requested_layouts.clear();overrides.clear();source_revisions.clear();pending_source_command.reset();canvas->clear_document();show_translation({});headings();frame->SetTitle(wxString::FromUTF8(product::name));break;
        case Command::Previous:go(page-1);break;case Command::Next:go(page+1);break;
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
        case Command::Copy:case Command::SelectAll:
            if(auto* native=wxDynamicCast(wxWindow::FindFocus(),wxTextCtrl)){if(command==Command::Copy)native->Copy();else native->SelectAll();}
            else if(wxWindow::FindFocus()==canvas){if(command==Command::Copy)canvas->copy_selection();else canvas->select_all();}
            break;
        case Command::CopySource:case Command::EditSource:
            if(command==Command::EditSource)reading_touched=true;
            if(command==Command::EditSource&&!loaded_sources.contains(page)){
                pending_source_command=std::pair{page,command};request_source(page);request_layout(page);break;
            }
            if(auto layout=current_layout()){
                if(command==Command::CopySource){if(!copy_text(layout->reading_text_utf8))frame->SetStatusText(wxS("クリップボードを開けません。もう一度お試しください。"),0);}
                else edit_source(*layout);
            }else{pending_source_command=std::pair{page,command};request_layout(page);frame->SetStatusText(wxS("原文を抽出しています…"),0);}
            break;
        case Command::Find:show_search();break;
        case Command::FindNext:case Command::FindPrevious:
            if(!search_bar->IsShown())show_search();
            if(search_input->GetValue().empty()){search_input->SetFocus();break;}
            begin_search(command==Command::FindPrevious,true);break;
        case Command::CloseSearch:
            cancel_search();search_bar->Hide();left_container->Layout();canvas->SetFocus();break;
        case Command::Translate:case Command::Retranslate:
            reading_touched=true;schedule_translation(true,command==Command::Retranslate);if(model_state!=ModelState::Ready)model_settings();break;
        case Command::TranslateAll:
            reading_touched=true;
            if(model_state==ModelState::Ready)start_batch();else{model_settings();batch_when_ready=model_state==ModelState::Loading;}break;
        case Command::Stop:
            stop_batch();
            translate_timer.Stop();translation_requested=false;force_requested=false;auto_suppressed=current_source_identity();
            scheduled_page.reset();pending_attempt.reset();inference->cancel_job();if(attempt){attempt->interrupt();save_attempt();refresh_translation();}frame->SetStatusText(wxS("停止しました。明示的な翻訳操作で再開できます。"),2);break;
        case Command::Automatic:
            auto_translate=!auto_translate;save_preferences();if(auto_translate){auto_suppressed.reset();schedule_translation();}break;
        case Command::Plain:case Command::Polite:{
            const auto selected=command==Command::Plain?Form::Plain:Form::Polite;
            if(form!=selected){form=selected;++settings_revision;stop_batch(true);invalidate_translation();save_preferences();schedule_translation();}break;
        }
        case Command::ModelSettings:model_settings();break;
        case Command::CpuRetry:backend_preference=BackendPreference::Cpu;load_model();break;
        case Command::ExportPage:case Command::ExportComplete:{
            wxFileDialog dialog(frame,wxS("訳文をUTF-8テキストへ書き出す"),{},wxS("translation.txt"),wxS("テキスト (*.txt)|*.txt"),wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
            if(dialog.ShowModal()!=wxID_OK)break;
            if(command==Command::ExportPage){const auto* shown=current_attempt();if(shown)storage_result(io->export_current(native_path(dialog.GetPath()),page,shown->display_text(),shown->state()));}
            else {CacheContext context;context.document_key=document_key;context.model_key=model_key;context.profile_digest=profile_digest(model_profile,form,model_capacity);storage_result(io->export_complete(native_path(dialog.GetPath()),std::move(context)));}break;
        }
        case Command::ClearCache:stop_batch(true);if(io)storage_result(io->clear_cache());break;
        case Command::Help:
            wxMessageBox(wxS("PDFを開くか、ウィンドウへドロップしてください。\nページ番号はEnterで確定します。\n\nホイール: ページ内をスクロール\nShift＋ホイール: 横スクロール\nPrimary＋ホイール: 拡大・縮小\nSpace＋ドラッグ／中ボタン: 移動\nF6: ペイン間のフォーカス移動\n\nPDFの原本は変更しません。"),wxS("操作方法"),wxOK|wxICON_INFORMATION,frame);break;
        case Command::About:
            wxMessageBox(wxString::FromUTF8(product::name)+" "+wxString::FromUTF8(product::version)+wxS("\nオフラインPDF翻訳アプリ\nMIT License"),wxS("このアプリについて"),wxOK,frame);break;
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
        auto file_menu=new wxMenu;file_menu->Append(Open,wxS("PDFを開く…\tCtrl+O"));recent_menu=new wxMenu;file_menu->AppendSubMenu(recent_menu,wxS("最近使ったPDF"));file_menu->Append(Close,wxS("文書を閉じる\tCtrl+W"));
        file_menu->AppendSeparator();file_menu->Append(ExportPage,wxS("現在ページの訳文を書き出す…"));file_menu->Append(ExportComplete,wxS("完成したページの訳文を書き出す…"));
        file_menu->AppendSeparator();file_menu->Append(ClearCache,wxS("翻訳キャッシュを削除"));file_menu->AppendSeparator();file_menu->Append(wxID_EXIT,wxS("終了\tCtrl+Q"));
        auto edit_menu=new wxMenu;edit_menu->Append(wxID_COPY,wxS("コピー\tCtrl+C"));edit_menu->Append(wxID_SELECTALL,wxS("全選択\tCtrl+A"));
        edit_menu->AppendSeparator();edit_menu->Append(CopySource,wxS("ページの原文をコピー"));edit_menu->Append(EditSource,wxS("翻訳用原文の確認・編集…\tCtrl+Shift+E"));
        edit_menu->AppendSeparator();edit_menu->Append(Find,wxS("PDF内を検索…\tCtrl+F"));edit_menu->Append(FindNext,wxS("次の検索結果\tCtrl+G"));edit_menu->Append(FindPrevious,wxS("前の検索結果\tCtrl+Shift+G"));
        auto view_menu=new wxMenu;
        view_menu->Append(FitWidth,wxS("幅に合わせる"));view_menu->Append(FitPage,wxS("ページ全体"));
        view_menu->Append(Left,wxS("左回転"));view_menu->Append(Right,wxS("右回転"));view_menu->Append(Equal,wxS("左右を均等にする"));
#ifdef __WXMAC__
        view_menu->Append(Previous,wxS("前のページ\tCtrl+["));view_menu->Append(Next,wxS("次のページ\tCtrl+]"));
#else
        view_menu->Append(Previous,wxS("前のページ\tAlt+PageUp"));view_menu->Append(Next,wxS("次のページ\tAlt+PageDown"));
#endif
        auto help_menu=new wxMenu;help_menu->Append(Help,wxS("操作方法"));help_menu->Append(About,wxS("このアプリについて"));
        auto translate_menu=new wxMenu;
        translate_menu->Append(Translate,wxS("このページを翻訳\tCtrl+T"));translate_menu->Append(Retranslate,wxS("このページを最初から再翻訳\tCtrl+Shift+T"));
        translate_menu->Append(TranslateAll,wxS("全ページを翻訳"));
        translate_menu->Append(Stop,wxS("停止\tCtrl+."));translate_menu->AppendCheckItem(Automatic,wxS("現在ページを自動翻訳"));translate_menu->Check(Automatic,true);
        translate_menu->AppendSeparator();translate_menu->AppendRadioItem(Plain,wxS("常体"));translate_menu->AppendRadioItem(Polite,wxS("敬体"));translate_menu->Check(Plain,true);
        translate_menu->AppendSeparator();translate_menu->Append(ModelSettings,wxS("モデル設定…\tCtrl+,"));translate_menu->Append(CpuRetry,wxS("CPUでモデルを読み直す"));
        auto menu=new wxMenuBar;menu->Append(file_menu,wxS("ファイル"));menu->Append(edit_menu,wxS("編集"));menu->Append(view_menu,wxS("表示"));menu->Append(translate_menu,wxS("翻訳"));menu->Append(help_menu,wxS("ヘルプ"));frame->SetMenuBar(menu);
        route(Open,Command::Open);route(Close,Command::Close);route(Previous,Command::Previous);route(Next,Command::Next);
        route(GoPage,Command::GoPage);route(FitWidth,Command::FitWidth);route(FitPage,Command::FitPage);route(Zoom,Command::Zoom);
        route(Left,Command::Left);route(Right,Command::Right);route(Equal,Command::Equal);route(Help,Command::Help);route(About,Command::About);
        route(wxID_COPY,Command::Copy);route(wxID_SELECTALL,Command::SelectAll);route(wxID_EXIT,Command::Quit);
        route(CopySource,Command::CopySource);route(EditSource,Command::EditSource);
        route(Find,Command::Find);route(FindNext,Command::FindNext);route(FindPrevious,Command::FindPrevious);route(CloseSearch,Command::CloseSearch);
        route(Translate,Command::Translate);route(Retranslate,Command::Retranslate);route(Stop,Command::Stop);route(Automatic,Command::Automatic);
        route(TranslateAll,Command::TranslateAll);
        route(Plain,Command::Plain);route(Polite,Command::Polite);route(ModelSettings,Command::ModelSettings);route(CpuRetry,Command::CpuRetry);
        route(ExportPage,Command::ExportPage);route(ExportComplete,Command::ExportComplete);route(ClearCache,Command::ClearCache);
        frame->Bind(wxEVT_UPDATE_UI,[this](wxUpdateUIEvent& e){e.Check(auto_translate);},Automatic);
        frame->Bind(wxEVT_UPDATE_UI,[this](wxUpdateUIEvent& e){e.Check(form==Form::Plain);},Plain);
        frame->Bind(wxEVT_UPDATE_UI,[this](wxUpdateUIEvent& e){e.Check(form==Form::Polite);},Polite);
        auto root=new wxBoxSizer(wxVERTICAL);
        toolbar=new wxPanel(frame);auto tools=new wxWrapSizer(wxHORIZONTAL);
        const int gap=frame->FromDIP(4);
        auto button=[&](int id,const wxString& label){auto* b=new wxButton(toolbar,id,label);tools->Add(b,0,wxALL,gap);return b;};
        open_button=button(Open,wxS("PDFを開く"));button(Previous,wxS("前へ"));button(Next,wxS("次へ"));
        page_input=new wxTextCtrl(toolbar,GoPage,{},wxDefaultPosition,frame->FromDIP(wxSize(68,-1)),wxTE_PROCESS_ENTER);page_input->SetName(wxS("ページ番号"));
        tools->Add(page_input,0,wxALL|wxALIGN_CENTER_VERTICAL,gap);
        page_count_label=new wxStaticText(toolbar,wxID_ANY,"/ 0");tools->Add(page_count_label,0,wxALL|wxALIGN_CENTER_VERTICAL,gap);
        wxArrayString options;for(const auto* label:{wxS("幅に合わせる"),wxS("ページ全体"),wxS("50%"),wxS("75%"),wxS("100%"),wxS("125%"),wxS("150%"),wxS("200%"),wxS("400%")})options.Add(label);
        zoom_input=new wxComboBox(toolbar,Zoom,wxS("幅に合わせる"),wxDefaultPosition,frame->FromDIP(wxSize(160,-1)),options,wxTE_PROCESS_ENTER);zoom_input->SetName(wxS("PDFの倍率"));tools->Add(zoom_input,0,wxALL|wxALIGN_CENTER_VERTICAL,gap);
        button(Translate,wxS("このページを翻訳"));button(Stop,wxS("停止"));button(ModelSettings,wxS("モデル設定"));
        toolbar->SetSizer(tools);root->Add(toolbar,0,wxEXPAND);
        split=new wxSplitterWindow(frame,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxSP_LIVE_UPDATE|wxSP_3D);
        auto left=new wxPanel(split);auto right=new wxPanel(split);auto ls=new wxBoxSizer(wxVERTICAL);auto rs=new wxBoxSizer(wxVERTICAL);
        left_container=left;
        left_heading=new wxStaticText(left,wxID_ANY,wxS("原文"));right_heading=new wxStaticText(right,wxID_ANY,wxS("訳文"));
        ls->Add(left_heading,0,wxALL,gap);rs->Add(right_heading,0,wxALL,gap);
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
        canvas=new PdfCanvas(left,[this](ViewRequest request){pdf->viewport(std::move(request));});ls->Add(canvas,1,wxEXPAND);
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
        translation->SetMaxLength(0);
        translation->Bind(wxEVT_MOUSEWHEEL,[touch](wxMouseEvent& e){touch();e.Skip();});translation->Bind(wxEVT_LEFT_DOWN,[touch](wxMouseEvent& e){touch();e.Skip();});
        auto font=translation->GetFont();if(font_ok)font.SetFaceName("Noto Sans CJK JP");font.SetPointSize(14);translation->SetFont(font);rs->Add(translation,1,wxEXPAND);
        left->SetSizer(ls);right->SetSizer(rs);split->SplitVertically(left,right,frame->GetClientSize().x/2);split->SetSashGravity(0.5);split->SetMinimumPaneSize(frame->FromDIP(120));root->Add(split,1,wxEXPAND);frame->SetSizer(root);
        frame->CreateStatusBar(3);int widths[]{-2,-1,-1};frame->SetStatusWidths(3,widths);frame->SetStatusText(wxS("モデル未設定"),1);
        if(!font_ok)frame->SetStatusText(wxS("同梱フォントを登録できませんでした。"),2);
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
            if(!closing){save_reading_position();save_preferences();stop_batch();invalidate_translation();closing=true;translate_timer.Stop();stream_timer.Stop();position_timer.Stop();search_timer.Stop();pdf->stop();inference->stop();if(io)io->stop();frame->Disable();frame->SetStatusText(wxS("終了処理中…"),0);close_timer.Start(50);}
            if(e.CanVeto())e.Veto();
        });
        headings();frame->Show();frame->Layout();split->SetSashPosition(split->GetClientSize().x/2);
        stream_timer.Start(50);
        position_timer.Start(1000);
        const auto directory=native_path(wxStandardPaths::Get().GetUserLocalDataDir());
        io=std::make_unique<IoService>(directory,directory/"Cache",[this](auto event){if(io_inbox.push(std::move(event)))wxQueueEvent(&owner,new wxThreadEvent(IO_WAKE));});
        if(!initial.empty())open_path(initial);
    }
};
ApplicationController::ApplicationController():impl_(std::make_unique<Impl>(*this)){}
ApplicationController::~ApplicationController()=default;
void ApplicationController::start(const wxString& initial){impl_->start(initial);}
}
