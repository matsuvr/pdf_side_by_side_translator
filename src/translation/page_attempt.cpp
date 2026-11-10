#include "translation/page_attempt.hpp"
#include "core/utf8.hpp"
#include "platform/digest.hpp"
#include <algorithm>
#include <stdexcept>

namespace opt {
namespace {
bool leaf(const AttemptChunk& c){return c.state!=LeafState::Replaced;}
bool success(const AttemptChunk& c){return c.state==LeafState::Complete||c.state==LeafState::SourceOnly;}
}
std::string_view finish_reason_label(FinishReason r){
    switch(r){
    case FinishReason::Completed:return "完了";case FinishReason::Cancelled:return "停止";
    case FinishReason::OutputLimit:return "生成上限";case FinishReason::ContextLimit:return "入力がcontext容量を超過";
    case FinishReason::EmptyOutput:return "空の出力";case FinishReason::InvalidEncoding:return "不正なUTF-8出力";
    case FinishReason::InvalidModelOutput:return "予期しないモデル出力";case FinishReason::ModelInputUnsupported:return "モデルの予約文字列を含む原文";
    case FinishReason::DecodeError:return "推論処理の失敗";case FinishReason::ModelUnavailable:return "モデルを利用できません";
    }return "不明な終了理由";
}
std::vector<AttemptBlock> PageAttempt::sources(const PageLayout& layout,std::shared_ptr<const std::string> edited){
    if(edited)return {{"edited-page",BlockKind::Edited,std::move(edited),false}};
    std::vector<AttemptBlock> result;result.reserve(layout.blocks.size());
    for(const auto& b:layout.blocks){
        const bool only=b.kind==BlockKind::Equation||b.kind==BlockKind::Table;
        auto source=std::make_shared<const std::string>(only?b.reading_text_utf8:b.translation_input_utf8);
        if(!source->empty())result.push_back({b.block_id,b.kind,std::move(source),only});
    }return result;
}
PageAttempt::PageAttempt(WorkStamp base,std::string key,std::vector<AttemptBlock> blocks,Form form,bool force,std::optional<AttemptSnapshot> saved)
    :base_(std::move(base)),blocks_(std::move(blocks)),form_(form){
    data_.id=new_uuid();data_.condition_key=std::move(key);data_.bypass_cache=force;
    bool translatable=false;
    for(const auto& b:blocks_){
        if(!b.source)throw std::invalid_argument("Attempt block has no source");
        if(b.source->empty())continue;
        AttemptChunk c;c.id=new_uuid();c.block_id=b.id;c.end=b.source->size();
        if(b.source_only||!has_usable_text(*b.source)){c.state=LeafState::SourceOnly;c.text=*b.source;}
        else translatable=true;
        data_.chunks.push_back(std::move(c));
    }
    if(saved&&!force&&saved->condition_key==data_.condition_key){
        // A corrupt/incompatible manifest must never grant completion. Check
        // every source interval and exact contiguous coverage before adopting.
        bool valid=!saved->id.empty()&&saved->plan>0;
        try{
        for(const auto& b:blocks_){
            std::size_t at=0;
            for(const auto& c:saved->chunks)if(c.block_id==b.id&&leaf(c)){
                if(c.begin!=at||c.end<=c.begin||c.end>b.source->size()||
                   (c.state==LeafState::Complete&&(!has_usable_text(c.text)||c.source_digest!=sha256(std::string_view(*b.source).substr(c.begin,c.end-c.begin)))))valid=false;
                if(c.state==LeafState::Complete)utf8_scalars(c.text);
                if(c.state==LeafState::SourceOnly&&c.text!=b.source->substr(c.begin,c.end-c.begin))valid=false;
                at=c.end;
            }
            if(at!=b.source->size())valid=false;
        }
        for(const auto& c:saved->chunks)if(std::none_of(blocks_.begin(),blocks_.end(),[&](const auto& b){return b.id==c.block_id;}))valid=false;
        }catch(const std::exception&){valid=false;}
        if(valid){data_=std::move(*saved);resume();}
    }
    base_.page_attempt=data_.id;
    settle();if(!translatable)data_.state=PageState::NoText;
}
bool PageAttempt::accepts(const WorkStamp& stamp)const{return active_&&stamp==active_stamp_;}
std::optional<ChunkRequest> PageAttempt::next_request(){
    if(active_)return {};
    const auto found=std::find_if(data_.chunks.begin(),data_.chunks.end(),[](const auto& c){return c.state==LeafState::Pending;});
    if(found==data_.chunks.end()){settle();return {};}
    active_=static_cast<std::size_t>(found-data_.chunks.begin());
    auto& c=*found;c.state=LeafState::Planning;c.plan=data_.plan;c.text.clear();
    active_stamp_=base_;active_stamp_.plan=data_.plan;active_stamp_.job=++next_job_;
    const auto b=std::find_if(blocks_.begin(),blocks_.end(),[&](const auto& b){return b.id==c.block_id;});
    data_.state=PageState::Running;
    return ChunkRequest{active_stamp_,c.block_id,b->source,c.begin,c.end,c.target_tokens,form_};
}
bool PageAttempt::prepared(std::shared_ptr<const PreparedChunk> p){
    if(!p||!accepts(p->stamp))return false;
    const auto index=*active_;auto& c=data_.chunks[index];
    if(p->block_id!=c.block_id||p->source_begin!=c.begin||p->source_end<=c.begin||p->source_end>c.end)return false;
    const auto remainder_end=c.end;c.end=p->source_end;c.id=p->chunk_id;c.source_digest=p->source_digest;
    c.prompt_digest=p->prompt_ids_digest;c.plan_digest=p->plan_digest;c.source_tokens=p->input.source_tokens;
    c.prompt_tokens=static_cast<std::uint32_t>(p->input.tokens.size());c.output_limit=p->input.output_limit;c.state=LeafState::Running;
    if(remainder_end>c.end){
        AttemptChunk remainder;remainder.id=new_uuid();remainder.parent_id=c.parent_id;remainder.block_id=c.block_id;
        remainder.begin=c.end;remainder.end=remainder_end;remainder.target_tokens=c.target_tokens;remainder.split_retry=c.split_retry;
        data_.chunks.insert(data_.chunks.begin()+index+1,std::move(remainder));
    }
    prepared_=std::move(p);
    if(prepared_->source_only){
        auto& current=data_.chunks[index];current.state=LeafState::SourceOnly;
        const auto b=std::find_if(blocks_.begin(),blocks_.end(),[&](const auto& b){return b.id==current.block_id;});
        current.text=b->source->substr(current.begin,current.end-current.begin);active_.reset();prepared_.reset();settle();
    }else if(prepared_->failure)finished(active_stamp_,{*prepared_->failure,{},{}});
    return true;
}
bool PageAttempt::delta(const WorkStamp& stamp,std::string_view text){
    if(!accepts(stamp))return false;
    // Service delivers valid complete UTF-8 prefixes. Keep the boundary honest
    // for injected backends and future adapters too.
    utf8_scalars(text);data_.chunks[*active_].text.append(text);return true;
}
bool PageAttempt::split_limited(std::size_t index){
    auto& parent=data_.chunks[index];
    if(parent.split_retry||parent.end-parent.begin<2)return false;
    const auto b=std::find_if(blocks_.begin(),blocks_.end(),[&](const auto& b){return b.id==parent.block_id;});
    const auto boundary=source_split_boundary(std::string_view(*b->source).substr(parent.begin,parent.end-parent.begin));
    if(!boundary)return false;
    // Hard upper range ensures even one very large token is actually split.
    // The planner subsequently keeps clusters where context permits.
    const auto middle=parent.begin+boundary;
    const auto begin=parent.begin,end=parent.end;const auto parent_id=parent.id,block=parent.block_id;
    const auto target=std::max(1U,parent.source_tokens/2);
    parent.state=LeafState::Replaced;parent.reason=FinishReason::OutputLimit;
    // Old partial is no longer displayed or concatenated with its children.
    parent.text.clear();++data_.plan;
    AttemptChunk first;first.id=new_uuid();first.parent_id=parent_id;first.block_id=block;
    first.begin=begin;first.end=middle;first.target_tokens=target;first.plan=data_.plan;first.split_retry=true;
    auto second=first;second.id=new_uuid();second.begin=middle;second.end=end;
    data_.chunks.insert(data_.chunks.begin()+index+1,{std::move(first),std::move(second)});return true;
}
bool PageAttempt::finished(const WorkStamp& stamp,const GenerationResult& result){
    if(!accepts(stamp))return false;
    const auto index=*active_;auto& c=data_.chunks[index];c.text=result.text;c.reason=result.reason;c.output_tokens=result.output_tokens;
    if(result.reason==FinishReason::Completed){
        try{utf8_scalars(result.text);c.state=has_usable_text(result.text)?LeafState::Complete:LeafState::Failed;}
        catch(const Utf8Error&){c.state=LeafState::Failed;c.reason=FinishReason::InvalidEncoding;c.text.clear();}
        if(c.state==LeafState::Failed&&c.reason==FinishReason::Completed)c.reason=FinishReason::EmptyOutput;
    }else c.state=c.text.empty()?LeafState::Failed:LeafState::Partial;
    if(result.reason==FinishReason::OutputLimit)split_limited(index);
    active_.reset();prepared_.reset();settle();return true;
}
bool PageAttempt::preparation_failed(const WorkStamp& stamp,FinishReason reason){return finished(stamp,{reason,{},{}});}
void PageAttempt::interrupt(){
    if(!active_&&(data_.state==PageState::Complete||data_.state==PageState::NoText))return;
    if(active_){auto& c=data_.chunks[*active_];c.state=c.text.empty()?LeafState::Pending:LeafState::Partial;c.reason=FinishReason::Cancelled;}
    active_.reset();prepared_.reset();active_stamp_.job=++next_job_;
    data_.state=PageState::Partial;
}
void PageAttempt::resume(){
    active_.reset();prepared_.reset();
    for(auto& c:data_.chunks)if(leaf(c)&&!success(c)){c.state=LeafState::Pending;c.text.clear();}
    settle();
}
void PageAttempt::settle(){
    bool pending=false,failed=false,translated=false,any=false;
    for(const auto& c:data_.chunks)if(leaf(c)){
        pending|=c.state==LeafState::Pending||c.state==LeafState::Planning||c.state==LeafState::Running;
        failed|=c.state==LeafState::Failed||c.state==LeafState::Partial;
        translated|=c.state==LeafState::Complete||!c.text.empty();any|=c.state!=LeafState::SourceOnly;
    }
    data_.planning_complete=!pending;
    data_.state=pending?PageState::Queued:failed?(translated?PageState::Partial:PageState::Failed):any?PageState::Complete:PageState::NoText;
}
std::string PageAttempt::display_text()const{
    std::string result;
    for(const auto& b:blocks_){
        std::string text;
        for(const auto& c:data_.chunks)if(c.block_id==b.id&&leaf(c)){
            text+=c.text;
            if(c.state==LeafState::Failed||c.state==LeafState::Partial){
                text+="\n［";text+=finish_reason_label(c.reason);text+="：この部分は未完了です］\n";
            }
        }
        if(text.empty())continue;
        if(!result.empty())result+="\n\n";
        if(b.source_only)result+=b.kind==BlockKind::Table?"［表・原文］\n":"［数式・原文］\n";
        result+=text;
    }return result;
}
}
