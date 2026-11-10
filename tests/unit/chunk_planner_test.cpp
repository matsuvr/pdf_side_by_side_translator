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
        const auto source=std::make_shared<const std::string>(std::string(5000,'x')+"\n\nSecond paragraph: 日本語 é 👩‍👩‍👧‍👦");
        opt::ChunkRequest request;request.block_id="page";request.source=source;
        unsigned calls=0;
        auto prompt=[&](std::string_view text,opt::Form form){
            check(text==*source,"Model received only part of the page");
            ++calls;
            return fake_prompt(text,form);
        };
        const auto first=opt::ChunkPlanner::prepare(request,8192,prompt);
        check(calls==1&&!first.failure&&first.source_begin==0&&first.source_end==source->size(),"Full page was not prepared once");
        check(first.input.tokens==fake_prompt(*source,request.form).tokens&&first.source_digest==opt::sha256(*source),"Page tokens or digest changed");
        const auto same=opt::ChunkPlanner::prepare(request,8192,prompt);
        check(first.plan_digest==same.plan_digest&&first.chunk_id!=same.chunk_id,"Execution UUID affected semantic identity");
        request.form=opt::Form::Polite;
        check(opt::ChunkPlanner::prepare(request,8192,prompt).prompt_ids_digest!=first.prompt_ids_digest,"Form change reused a different prompt's identity");
        const auto prompt_size=static_cast<std::uint32_t>(first.input.tokens.size());
        const auto near_limit=opt::ChunkPlanner::prepare(request,prompt_size+2,prompt);
        check(!near_limit.failure&&near_limit.input.output_limit==2,"Output reservation should use remaining context");
        const auto too_long=opt::ChunkPlanner::prepare(request,prompt_size,prompt);
        check(too_long.failure==opt::FinishReason::ContextLimit&&too_long.source_end==source->size(),"Oversized page was truncated or accepted");
        request.source=std::make_shared<const std::string>("A literal <|plamo:tag|> marker.");
        const auto reserved=opt::ChunkPlanner::prepare(request,8192,fake_prompt);
        check(reserved.failure==opt::FinishReason::ModelInputUnsupported,"Reserved input was accepted");
        request.source=std::make_shared<const std::string>(" \t　� ");
        const auto no_text=opt::ChunkPlanner::prepare(request,3,prompt);
        check(no_text.source_only&&!no_text.failure&&no_text.input.tokens.empty(),"Whitespace/replacement-only input was sent to the model");
        std::cout<<"Full-page prompts, actual context, stable identity and unsupported input checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
