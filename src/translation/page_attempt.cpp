#include "translation/page_attempt.hpp"
#include "core/utf8.hpp"
#include "platform/digest.hpp"
#include <stdexcept>

namespace opt {
namespace {
bool matches_page(const AttemptSnapshot& saved,const AttemptSnapshot& fresh,std::string_view source){
    if(saved.condition_key!=fresh.condition_key||saved.id.empty()||!saved.plan||
       saved.chunks.size()!=fresh.chunks.size())return false;
    if(saved.chunks.empty())return true;
    const auto& c=saved.chunks.front();
    if(c.id.empty()||c.block_id!=fresh.chunks.front().block_id||c.begin!=0||c.end!=source.size()||
       c.state==LeafState::Replaced||c.state==LeafState::SourceOnly)return false;
    if(c.state!=LeafState::Complete)return true;
    try{
        return c.reason==FinishReason::Completed&&valid_translation(c.text)&&c.source_digest==sha256(source);
    }catch(const Utf8Error&){return false;}
}
}
std::string_view finish_reason_label(FinishReason r){
    switch(r){
    case FinishReason::Completed:return "完了";case FinishReason::Cancelled:return "停止";
    case FinishReason::OutputLimit:return "生成上限";case FinishReason::ContextLimit:return "ページ全文がモデルの入力容量を超えています";
    case FinishReason::EmptyOutput:return "空の出力";case FinishReason::InvalidEncoding:return "不正なUTF-8出力";
    case FinishReason::InvalidModelOutput:return "予期しないモデル出力";case FinishReason::ModelInputUnsupported:return "原文をモデル入力に変換できません";
    case FinishReason::DecodeError:return "推論処理の失敗";case FinishReason::ModelUnavailable:return "モデルを利用できません";
    }return "不明な終了理由";
}
std::shared_ptr<const std::string> PageAttempt::source(const PageLayout& layout){
    return std::make_shared<const std::string>(layout.reading_text_utf8);
}
PageAttempt::PageAttempt(WorkStamp base,std::string key,std::shared_ptr<const std::string> source,Form form,
                         std::optional<AttemptSnapshot> saved,bool resume_unfinished)
    :base_(std::move(base)),source_(std::move(source)),form_(form){
    if(!source_)throw std::invalid_argument("Attempt has no page source");
    data_.id=new_uuid();data_.condition_key=std::move(key);
    if(has_usable_text(*source_)){
        AttemptChunk page;
        page.id=new_uuid();page.block_id="page";page.end=source_->size();
        data_.chunks.push_back(std::move(page));
    }
    if(saved&&matches_page(*saved,data_,*source_)){
        data_=std::move(*saved);
        if(resume_unfinished)resume();
    }
    base_.page_attempt=data_.id;
    settle();
}
bool PageAttempt::accepts(const WorkStamp& stamp)const{return active_&&stamp==active_stamp_;}
std::optional<ChunkRequest> PageAttempt::next_request(){
    if(active_||data_.chunks.empty())return {};
    auto& page=data_.chunks.front();
    if(page.state!=LeafState::Pending)return {};
    active_=true;page.state=LeafState::Planning;page.text.clear();page.plan=data_.plan;
    active_stamp_=base_;active_stamp_.plan=data_.plan;active_stamp_.job=++next_job_;
    data_.state=PageState::Running;
    return ChunkRequest{active_stamp_,page.block_id,source_,form_};
}
bool PageAttempt::prepared(std::shared_ptr<const PreparedChunk> p){
    if(!p||!accepts(p->stamp))return false;
    auto& c=data_.chunks.front();
    if(p->block_id!=c.block_id||p->source_begin!=0||p->source_end!=source_->size()||p->source_only)return false;
    c.id=p->chunk_id;c.source_digest=p->source_digest;
    c.prompt_digest=p->prompt_ids_digest;c.plan_digest=p->plan_digest;c.source_tokens=p->input.source_tokens;
    c.prompt_tokens=static_cast<std::uint32_t>(p->input.tokens.size());c.output_limit=p->input.output_limit;c.state=LeafState::Running;
    prepared_=std::move(p);
    if(prepared_->failure)finished(active_stamp_,{*prepared_->failure,{},{}});
    return true;
}
bool PageAttempt::delta(const WorkStamp& stamp,std::string_view text){
    if(!accepts(stamp))return false;
    // Service delivers valid complete UTF-8 prefixes. Keep the boundary honest
    // for injected backends and future adapters too.
    if(text.size()>max_translation_bytes-data_.chunks.front().text.size())return false;
    try{validate_utf8(text);}catch(const Utf8Error&){return false;}
    if(unsupported_text_control(text)!=std::string_view::npos)return false;
    data_.chunks.front().text.append(text);return true;
}
bool PageAttempt::finished(const WorkStamp& stamp,const GenerationResult& result){
    if(!accepts(stamp))return false;
    auto& c=data_.chunks.front();c.reason=result.reason;c.output_tokens=result.output_tokens;
    if(result.text.size()>max_translation_bytes)c.reason=FinishReason::OutputLimit;
    else{
        try{validate_utf8(result.text);c.text=result.text;}
        catch(const Utf8Error&){c.reason=FinishReason::InvalidEncoding;} // Keep the last valid streamed prefix.
    }
    const auto control=unsupported_text_control(c.text);
    if(control!=std::string::npos){c.text.resize(control);c.reason=FinishReason::InvalidModelOutput;c.state=c.text.empty()?LeafState::Failed:LeafState::Partial;}
    else if(c.reason==FinishReason::Completed){
        c.state=has_usable_text(c.text)?LeafState::Complete:LeafState::Failed;
        if(c.state==LeafState::Failed)c.reason=FinishReason::EmptyOutput;
    }else c.state=c.text.empty()?LeafState::Failed:LeafState::Partial;
    active_=false;prepared_.reset();settle();return true;
}
bool PageAttempt::preparation_failed(const WorkStamp& stamp,FinishReason reason){return finished(stamp,{reason,{},{}});}
void PageAttempt::interrupt(){
    if(!active_&&(data_.state==PageState::Complete||data_.state==PageState::NoText))return;
    if(active_){auto& c=data_.chunks.front();c.state=c.text.empty()?LeafState::Pending:LeafState::Partial;c.reason=FinishReason::Cancelled;}
    active_=false;prepared_.reset();active_stamp_.job=++next_job_;
    data_.state=PageState::Partial;
}
void PageAttempt::resume(){
    active_=false;prepared_.reset();
    if(!data_.chunks.empty()){
        auto& page=data_.chunks.front();
        if(page.state!=LeafState::Complete){
            // Restoring an attempt also restarts its job counter. Keep callbacks
            // from the interrupted generation distinct from the next full-page run.
            ++data_.plan;
            page.state=LeafState::Pending;page.text.clear();
        }
    }
    settle();
}
void PageAttempt::settle(){
    data_.planning_complete=true;
    if(data_.chunks.empty()){
        data_.state=PageState::NoText;
        return;
    }
    switch(data_.chunks.front().state){
    case LeafState::Complete:data_.state=PageState::Complete;break;
    case LeafState::Failed:data_.state=PageState::Failed;break;
    case LeafState::Partial:data_.state=PageState::Partial;break;
    default:data_.state=PageState::Queued;data_.planning_complete=false;break;
    }
}
std::string PageAttempt::display_text()const{
    if(data_.chunks.empty())return {};
    const auto& page=data_.chunks.front();
    auto text=page.text;
    if(page.state==LeafState::Failed||page.state==LeafState::Partial){
        text+="\n［";text+=finish_reason_label(page.reason);text+="：このページの翻訳は未完了です］\n";
    }
    return text;
}
}
