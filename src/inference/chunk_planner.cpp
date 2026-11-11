#include "inference/chunk_planner.hpp"
#include "core/utf8.hpp"
#include "platform/digest.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace opt {
namespace {
std::string prompt_digest(const PreparedInput& input){
    std::vector<std::byte> bytes;bytes.reserve(input.tokens.size()*4);
    for(const auto token:input.tokens){
        const auto bits=std::bit_cast<std::uint32_t>(token);
        for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::byte>((bits>>(i*8))&255));
    }
    return CanonicalKey{}.text("format","prompt-ids-i32le-v1").number("count",input.tokens.size()).bytes("ids",bytes).finish();
}
}
PreparedChunk ChunkPlanner::prepare(const ChunkRequest& request,std::uint32_t capacity,const Prepare& prompt,
                                   const std::function<bool()>& cancelled){
    const auto checkpoint=[&]{if(cancelled&&cancelled())throw PreparationCancelled();};
    checkpoint();
    if(!request.source)throw std::invalid_argument("Missing page source");
    const auto& source=*request.source;
    PreparedChunk prepared;
    prepared.stamp=request.stamp;
    prepared.block_id=request.block_id;
    prepared.chunk_id=new_uuid();
    prepared.source_end=source.size();
    const bool usable=has_usable_text(source,checkpoint);
    Sha256 digest;
    for(std::size_t offset=0;offset<source.size();){
        checkpoint();
        const auto count=std::min<std::size_t>(64*1024,source.size()-offset);
        digest.update(std::string_view(source).substr(offset,count));offset+=count;
    }
    prepared.source_digest=digest.finish();
    checkpoint();
    if(!usable){
        prepared.source_only=true;
    }else{
        try{
            prepared.input=prompt(source,request.form);
            checkpoint();
            const auto prompt_size=prepared.input.tokens.size();
            if(prompt_size>=capacity||prepared.input.output_limit==0){
                prepared.failure=FinishReason::ContextLimit;
                prepared.input={};
            }else{
                const auto remaining=capacity-static_cast<std::uint32_t>(prompt_size);
                prepared.input.output_limit=std::min(prepared.input.output_limit,remaining);
                prepared.prompt_ids_digest=prompt_digest(prepared.input);
            }
        }catch(const ModelInputUnsupported&){
            prepared.failure=FinishReason::ModelInputUnsupported;
        }catch(const InputContextLimit&){
            prepared.failure=FinishReason::ContextLimit;
        }
    }
    checkpoint();
    prepared.plan_digest=CanonicalKey{}.text("block",prepared.block_id).number("algorithm",prepared.chunk_algorithm_version)
        .number("begin",prepared.source_begin).number("end",prepared.source_end).text("source",prepared.source_digest)
        .text("prompt",prepared.prompt_ids_digest).finish();
    return prepared;
}
}
