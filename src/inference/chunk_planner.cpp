#include "inference/chunk_planner.hpp"
#include "core/utf8.hpp"
#include "platform/digest.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace opt {
namespace {
bool continuation(unsigned char c){return (c&0xc0)==0x80;}
bool extender(char32_t c){
    return (c>=0x300&&c<=0x36f)||(c>=0x1ab0&&c<=0x1aff)||(c>=0x1dc0&&c<=0x1dff)||
        (c>=0x20d0&&c<=0x20ff)||(c>=0xfe20&&c<=0xfe2f)||c==0xfe0e||c==0xfe0f||
        (c>=0x1f3fb&&c<=0x1f3ff);
}
bool regional(char32_t c){return c>=0x1f1e6&&c<=0x1f1ff;}
std::size_t cluster_boundary(const std::vector<Utf8Scalar>& scalars,std::size_t count){
    const auto original=count;
    auto safe=[&](std::size_t at){
        if(!at||at>=scalars.size())return true;
        const auto prior=scalars[at-1].value,next=scalars[at].value;
        if(extender(next)||next==0x200d||prior==0x200d)return false;
        if(regional(prior)&&regional(next)){
            std::size_t run=0;for(auto i=at;i>0&&regional(scalars[i-1].value);--i)++run;
            if(run%2)return false;
        }
        return true;
    };
    while(count>0&&!safe(count))--count;
    if(count)return count;
    // A first cluster may exceed the soft token target while still fitting
    // the actual context. Prefer keeping it before falling back to scalars.
    count=original;while(count<scalars.size()&&!safe(count))++count;return count;
}
std::size_t semantic_boundary(const std::vector<Utf8Scalar>& scalars,std::size_t count){
    if(count<2)return count;
    const auto minimum=scalars[count-1].end/2;
    std::size_t sentence=0,clause=0,word=0;
    for(std::size_t i=0;i<count;++i){
        if(scalars[i].end<minimum)continue;
        const auto c=scalars[i].value;
        if(unicode_space(c))word=i+1;
        if((c=='.'||c=='!'||c=='?'||c==0x3002||c==0xff01||c==0xff1f)&&
           (i+1==scalars.size()||unicode_space(scalars[i+1].value)))sentence=i+1;
        if(c==';'||c==':'||c==','||c=='\n'||c==0xff0c||c==0xff1b)clause=i+1;
    }
    return cluster_boundary(scalars,sentence?sentence:clause?clause:word?word:count);
}
std::string prompt_digest(const PreparedInput& input){
    std::vector<std::byte> bytes;bytes.reserve(input.tokens.size()*4);
    for(const auto token:input.tokens){
        const auto bits=std::bit_cast<std::uint32_t>(token);
        for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::byte>((bits>>(i*8))&255));
    }
    return CanonicalKey{}.text("format","prompt-ids-i32le-v1").number("count",input.tokens.size()).bytes("ids",bytes).finish();
}
}
std::size_t source_split_boundary(std::string_view source){
    const auto scalars=utf8_scalars(source);if(scalars.size()<2)return 0;
    const auto count=cluster_boundary(scalars,scalars.size()/2);
    return count>0&&count<scalars.size()?scalars[count-1].end:0;
}
PreparedChunk ChunkPlanner::prepare(const ChunkRequest& request,std::uint32_t capacity,
                                   const Prepare& prompt,const Count& count_tokens){
    if(!request.source)throw std::invalid_argument("Missing chunk source");
    const auto& source=*request.source;const auto end=std::min(request.end,source.size());
    if(request.begin>=end)throw std::invalid_argument("Empty or reversed source range");
    if(continuation(static_cast<unsigned char>(source[request.begin]))||
       (end<source.size()&&continuation(static_cast<unsigned char>(source[end]))))throw Utf8Error("Source range splits a UTF-8 scalar");
    auto window_end=request.begin+std::min<std::size_t>(end-request.begin,4096);
    while(window_end>request.begin&&window_end<end&&continuation(static_cast<unsigned char>(source[window_end])))--window_end;
    if(window_end==request.begin)throw Utf8Error("Cannot advance source range");
    auto scan_end=window_end+std::min<std::size_t>(end-window_end,4);
    while(scan_end>window_end&&scan_end<end&&continuation(static_cast<unsigned char>(source[scan_end])))--scan_end;
    const auto scalars=utf8_scalars(std::string_view(source).substr(request.begin,scan_end-request.begin));
    std::size_t count=0;while(count<scalars.size()&&request.begin+scalars[count].end<=window_end)++count;
    if(!count)throw Utf8Error("No complete scalar in chunk");
    const auto target=std::max(1U,request.target_source_tokens);
    if(count_tokens(std::string_view(source).substr(request.begin,scalars[count-1].end))>target){
        std::size_t low=1,high=count,fit=1;
        while(low<=high){
            const auto middle=low+(high-low)/2;
            const auto actual=count_tokens(std::string_view(source).substr(request.begin,scalars[middle-1].end));
            if(actual<=target){fit=middle;low=middle+1;}else high=middle-1;
        }
        count=fit;
    }
    if(request.begin+scalars[count-1].end<end)count=semantic_boundary(scalars,count);
    count=cluster_boundary(scalars,count);
    PreparedChunk prepared;prepared.stamp=request.stamp;prepared.block_id=request.block_id;prepared.chunk_id=new_uuid();prepared.source_begin=request.begin;
    while(true){
        prepared.source_end=request.begin+scalars[count-1].end;
        const auto input=std::string_view(source).substr(prepared.source_begin,prepared.source_end-prepared.source_begin);
        if(!has_usable_text(input)){prepared.source_only=true;break;}
        try{
            prepared.input=prompt(input,request.form);
        }catch(const ModelInputUnsupported&){prepared.failure=FinishReason::ModelInputUnsupported;break;}
        if(static_cast<std::uint64_t>(prepared.input.tokens.size())+prepared.input.output_limit<=capacity)break;
        if(count==1){prepared.failure=FinishReason::ContextLimit;break;}
        const auto next=cluster_boundary(scalars,count/2);count=std::max<std::size_t>(1,next<count?next:count-1);
    }
    const auto input=std::string_view(source).substr(prepared.source_begin,prepared.source_end-prepared.source_begin);
    prepared.source_digest=sha256(input);
    if(!prepared.failure&&!prepared.source_only)prepared.prompt_ids_digest=prompt_digest(prepared.input);
    prepared.plan_digest=CanonicalKey{}.text("block",prepared.block_id).number("algorithm",prepared.chunk_algorithm_version)
        .number("begin",prepared.source_begin).number("end",prepared.source_end).text("source",prepared.source_digest)
        .text("prompt",prepared.prompt_ids_digest).finish();
    return prepared;
}
}
