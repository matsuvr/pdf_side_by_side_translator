#include "inference/chunk_planner.hpp"
#include "inference/bounded_tokenizer.hpp"
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
        request.form=opt::Form::Easy;
        check(opt::ChunkPlanner::prepare(request,8192,prompt).prompt_ids_digest!=first.prompt_ids_digest,"Form change reused a different prompt's identity");
        const auto prompt_size=static_cast<std::uint32_t>(first.input.tokens.size());
        const auto near_limit=opt::ChunkPlanner::prepare(request,prompt_size+2,prompt);
        check(!near_limit.failure&&near_limit.input.output_limit==2,"Output reservation should use remaining context");
        const auto too_long=opt::ChunkPlanner::prepare(request,prompt_size,prompt);
        check(too_long.failure==opt::FinishReason::ContextLimit&&too_long.source_end==source->size(),"Oversized page was truncated or accepted");
        check(too_long.input.tokens.empty()&&too_long.input.tokens.capacity()==0,"Rejected token buffer was retained");
        const auto rejected=opt::ChunkPlanner::prepare(request,8192,[](std::string_view,opt::Form)->opt::PreparedInput{throw opt::InputContextLimit();});
        check(rejected.failure==opt::FinishReason::ContextLimit&&rejected.source_end==source->size(),"Bounded tokenizer overflow was not reported");
        request.source=std::make_shared<const std::string>("A literal <|plamo:tag|> marker.");
        const auto reserved=opt::ChunkPlanner::prepare(request,8192,fake_prompt);
        check(reserved.failure==opt::FinishReason::ModelInputUnsupported,"Reserved input was accepted");
        request.source=std::make_shared<const std::string>(" \t　� ");
        const auto no_text=opt::ChunkPlanner::prepare(request,3,prompt);
        check(no_text.source_only&&!no_text.failure&&no_text.input.tokens.empty(),"Whitespace/replacement-only input was sent to the model");
        request.source=std::make_shared<const std::string>("Usable prefix\xed\xa0\x80");
        bool invalid=false;
        try{opt::ChunkPlanner::prepare(request,8192,fake_prompt);}catch(const opt::Utf8Error&){invalid=true;}
        check(invalid,"Usable prefix hid invalid UTF-8 at the end");
        request.source=std::make_shared<const std::string>(8*1024*1024,'x');
        unsigned checkpoints=0;bool cancelled=false,called=false;
        try{
            opt::ChunkPlanner::prepare(request,8192,[&](std::string_view,opt::Form){called=true;return opt::PreparedInput{};},
                [&]{return ++checkpoints==4;});
        }catch(const opt::PreparationCancelled&){cancelled=true;}
        check(cancelled&&!called&&checkpoints==4,"Input scan ignored cancellation or entered the tokenizer");
        // A fixed context buffer is sufficient even when the tokenizer reports
        // a larger required size; no second call or oversized token allocation.
        unsigned encodes=0;
        auto one_byte=[&](std::string_view text,std::span<std::int32_t> out){
            ++encodes;
            std::size_t count=0;
            for(std::size_t i=0;i<text.size();){
                if(text.substr(i,3)=="\xee\xb8\x80"){i+=3;continue;}
                if(count<out.size())out[count]=static_cast<unsigned char>(text[i]);
                ++i;++count;
            }
            return static_cast<int>(count)*(count<=out.size()?1:-1);
        };
        bool overflow=false;
        try{opt::tokenize_plamo3_bounded(*request.source,8192,32,one_byte,[]{});}catch(const opt::InputContextLimit&){overflow=true;}
        check(overflow&&encodes==0,"Impossible input entered the allocating tokenizer");
        overflow=false;
        try{opt::tokenize_plamo3_bounded("abcdef",4,2,one_byte,[]{});}catch(const opt::InputContextLimit&){overflow=true;}
        check(overflow&&encodes==1,"Oversized tokenizer result was resized and retried");
        const auto segmented=opt::tokenize_plamo3_bounded("ab\xee\xb8\x80\xee\xb8\x80" "cd",4,2,one_byte,[]{});
        check(segmented==std::vector<std::int32_t>({'a','b','c','d'}),"PLaMo private boundary segmentation changed output");
        encodes=0;
        const auto empty=opt::tokenize_plamo3_bounded("\xee\xb8\x80\xee\xb8\x80",0,2,one_byte,[]{});
        check(empty.empty()&&encodes==0,"Empty boundary segments consumed context");
        const std::string spanning="before<|plamo:x\xee\xb8\x80y|>after";
        opt::tokenize_plamo3_bounded(spanning,256,48,[&](std::string_view input,std::span<std::int32_t> output){
            check(input==spanning,"Private boundaries split a PFML marker before upstream segmentation");
            return one_byte(input,output);
        },[]{});
        std::string long_run,retained_run;
        for(unsigned i=0;i<1000;++i)long_run+="\xee\xb8\x80";
        for(unsigned i=0;i<65;++i)retained_run+="\xee\xb8\x80";
        const auto compacted=opt::tokenize_plamo3_bounded("pre"+long_run+"post",7,1,
            [&](std::string_view input,std::span<std::int32_t> output){
                check(input=="pre"+retained_run+"post","Boundary compaction changed retained text or PFML length semantics");
                return one_byte(input,output);
            },[]{});
        check(compacted.size()==7,"Boundary-heavy input changed tokens");
        cancelled=false;checkpoints=0;
        try{opt::tokenize_plamo3_bounded("a\xee\xb8\x80" "b",4,2,one_byte,[&]{if(++checkpoints==4)throw opt::PreparationCancelled();});}
        catch(const opt::PreparationCancelled&){cancelled=true;}
        check(cancelled,"Bounded tokenization ignored cancellation");
        std::cout<<"Full-page prompts, context-bounded tokenization, stable identity and preparation cancellation passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
