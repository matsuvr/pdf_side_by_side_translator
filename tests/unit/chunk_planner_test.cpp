#include "inference/chunk_planner.hpp"
#include "core/utf8.hpp"
#include "platform/digest.hpp"
#include <iostream>
#include <stdexcept>

void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
opt::PreparedInput fake_prompt(std::string_view source,opt::Form form){
    if(source.find("<|plamo:tag|>")!=std::string_view::npos)throw opt::ModelInputUnsupported("reserved marker");
    opt::PreparedInput out;out.tokens={1,form==opt::Form::Plain?2:3};out.stop_ids={99};out.output_limit=4;
    const auto scalars=opt::utf8_scalars(source);out.source_tokens=static_cast<std::uint32_t>(scalars.size());
    for(auto scalar:scalars)out.tokens.push_back(static_cast<std::int32_t>(scalar.value));out.tokens.push_back(4);return out;
}
int main(){
    try{
        auto count=[](std::string_view source){return opt::utf8_scalars(source).size();};
        const auto source=std::make_shared<const std::string>("First sentence. Second sentence has 3 values, and a final clause. 日本語の文。 é 😀");
        opt::ChunkRequest request;request.block_id="paragraph";request.source=source;request.target_source_tokens=18;
        std::string restored;unsigned chunks=0;
        while(request.begin<source->size()){
            auto chunk=opt::ChunkPlanner::prepare(request,32,fake_prompt,count);
            check(!chunk.failure&&chunk.source_begin==request.begin&&chunk.source_end>chunk.source_begin,"Planner did not advance a complete source range");
            check(chunk.input.tokens.size()+chunk.input.output_limit<=32,"Final prompt exceeded actual context");
            const auto text=source->substr(chunk.source_begin,chunk.source_end-chunk.source_begin);
            check(chunk.input.tokens==fake_prompt(text,request.form).tokens,"Budgeted token vector differs from execution vector");
            check(chunk.source_digest==opt::sha256(text),"Source digest does not describe the actual slice");
            restored+=text;request.begin=chunk.source_end;++chunks;
        }
        check(restored==*source&&chunks>1,"Source bytes were lost, overlapped or truncated");
        request.begin=0;request.target_source_tokens=1024;
        const auto first=opt::ChunkPlanner::prepare(request,12,fake_prompt,count);
        check(!first.failure&&first.input.tokens.size()+first.input.output_limit<=12,"Small context was not adaptively split");
        const auto same=opt::ChunkPlanner::prepare(request,12,fake_prompt,count);
        check(first.plan_digest==same.plan_digest&&first.prompt_ids_digest==same.prompt_ids_digest&&first.chunk_id!=same.chunk_id,"Stable semantic plan and execution identity were conflated");
        request.form=opt::Form::Polite;
        check(opt::ChunkPlanner::prepare(request,12,fake_prompt,count).prompt_ids_digest!=first.prompt_ids_digest,"Form change reused a different prompt's identity");
        request.source=std::make_shared<const std::string>("👩‍👩‍👧‍👦 tail");request.target_source_tokens=3;request.form=opt::Form::Plain;
        const auto family=opt::ChunkPlanner::prepare(request,64,fake_prompt,count);
        check(request.source->substr(0,family.source_end)=="👩‍👩‍👧‍👦","Soft target split a ZWJ cluster that fits the context");
        request.source=std::make_shared<const std::string>("éx");request.target_source_tokens=1;
        check(opt::ChunkPlanner::prepare(request,64,fake_prompt,count).source_end==std::string("é").size(),"Combining sequence was unnecessarily split");
        request.source=std::make_shared<const std::string>("A literal <|plamo:tag|> marker.");request.target_source_tokens=1024;
        const auto reserved=opt::ChunkPlanner::prepare(request,8192,fake_prompt,count);
        check(reserved.failure==opt::FinishReason::ModelInputUnsupported&&reserved.source_end==request.source->size(),"Unsupported input was rewritten or lost");
        request.source=std::make_shared<const std::string>("x");
        check(opt::ChunkPlanner::prepare(request,3,fake_prompt,count).failure==opt::FinishReason::ContextLimit,"Impossible single-scalar prompt was labelled ready");
        request.source=std::make_shared<const std::string>(" \t　� ");
        const auto no_text=opt::ChunkPlanner::prepare(request,3,fake_prompt,count);
        check(no_text.source_only&&!no_text.failure&&no_text.input.tokens.empty(),"Whitespace/replacement-only input was sent to the model");
        std::cout<<"Chunk coverage, actual context, immutable prompt IDs, grapheme preferences and unsupported input checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
