#include "inference/engine.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: inference_probe model.gguf [cpu|auto] [token-fixture.tsv] [--prepare-only]\n"; return 2; }
    try {
        const bool prepare_only=argc>2&&std::string_view(argv[argc-1])=="--prepare-only";
        const int positional_count=argc-static_cast<int>(prepare_only);
        std::atomic_bool cancel{false};
        opt::Engine engine(cancel);
        engine.load(std::filesystem::u8path(argv[1]), positional_count < 3 || std::string(argv[2]) == "cpu");
        std::cout << "backend=" << engine.backend() << " context=" << engine.capacity() << '\n';
        if (positional_count > 3) {
            std::ifstream fixture(argv[3]);
            if (!fixture) throw std::runtime_error("Fixture not found");
            std::string line;
            int count = 0;
            while (std::getline(fixture, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream row(line);
                std::string name, form, source_hex, expected;
                std::getline(row,name,'\t'); std::getline(row,form,'\t');
                std::getline(row,source_hex,'\t'); std::getline(row,expected);
                std::string source;
                for (std::size_t i=0;i<source_hex.size();i+=2)
                    source += static_cast<char>(std::stoi(source_hex.substr(i,2),nullptr,16));
                std::vector<std::int32_t> ids;
                std::istringstream numbers(expected);
                int id; while(numbers>>id) ids.push_back(id);
                if(form!="plain"&&form!="easy") throw std::runtime_error("Unknown fixture style: "+form);
                auto actual=engine.prepare(source,opt::form_from_setting(form));
                if(ids!=actual.tokens) throw std::runtime_error("Tokenizer mismatch: "+name);
                ++count;
            }
            if (!count) throw std::runtime_error("Empty tokenizer fixture");
            std::cout << "reference_tokenizer_cases=" << count << " matched\n";
        }
        bool unsupported = false;
        try { engine.prepare("Print <|plamo:tag|> literally.",opt::Form::Plain); }
        catch (const std::runtime_error& e) { unsupported = true; std::cout << e.what() << '\n'; }
        std::cout << "reserved_marker=" << (unsupported?"unsupported":"literal") << '\n';
        const auto prefix=engine.tokenize("<|plamo:tag|>dataset<|plamo:msg|>translation"
            "<|plamo:tag|>input<|plamo:key|>lang<|plamo:val|>en<|plamo:msg|>",true,true);
        const auto suffix=engine.tokenize("<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>ja"
            "<|plamo:key|>form<|plamo:val|>plain<|plamo:msg|>",false,true);
        std::string boundary_run;
        for(unsigned i=0;i<1000;++i)boundary_run+="\xee\xb8\x80";
        const std::vector<std::string> boundary_cases={
            "ab\xee\xb8\x80" "cd", "\xee\xb8\x80\xee\xb8\x80", "日\xee\xb8\x80本語\xee\xb8\x80  text",
            "before<|plamo:x\xee\xb8\x80y|>after", "prefix<|plamo:"+boundary_run+"|>suffix", "before"+boundary_run+"after"};
        for(const auto& source:boundary_cases){
            auto expected=prefix;
            const auto body=engine.tokenize(source,false,false);
            expected.insert(expected.end(),body.begin(),body.end());expected.insert(expected.end(),suffix.begin(),suffix.end());
            if(engine.prepare(source,opt::Form::Plain).tokens!=expected)
                throw std::runtime_error("Bounded tokenizer changed private-boundary tokenization");
        }
        bool context_rejected=false;
        try{engine.prepare(std::string(8*1024*1024,'x'),opt::Form::Plain);}
        catch(const opt::InputContextLimit&){context_rejected=true;}
        if(!context_rejected)throw std::runtime_error("Oversized preparation did not reject context overflow");
        cancel=true;bool preparation_cancelled=false;
        try{engine.prepare("Input to cancel before tokenization.",opt::Form::Plain);}
        catch(const opt::PreparationCancelled&){preparation_cancelled=true;}
        cancel=false;
        if(!preparation_cancelled)throw std::runtime_error("Preparation ignored cancellation");
        std::cout << "bounded_preparation_private_boundaries_and_cancellation=passed\n";
        if(prepare_only)return 0;
        for (auto form : {opt::Form::Plain,opt::Form::Easy}) {
            std::cout << "style=" << opt::form_setting(form) << '\n';
            for (const auto* input : {"The device has 3 sensors. It does not use the network.",
                                      "This application translates documents offline."}) {
                auto prepared=engine.prepare(input,form);
                auto result=engine.generate(prepared,[](std::string_view delta){std::cout<<delta<<std::flush;});
                std::cout << "\nreason=" << static_cast<int>(result.reason) << " generated=" << result.output_tokens << '\n';
                if(result.reason!=opt::FinishReason::Completed) throw std::runtime_error("Incomplete generation");
            }
        }
        auto prepared=engine.prepare("The device has 3 sensors. It does not use the network.",opt::Form::Plain);
        auto baseline=engine.generate(prepared,[](std::string_view){});
        cancel=true;
        auto cancelled=engine.generate(prepared,[](std::string_view){});
        if(cancelled.reason!=opt::FinishReason::Cancelled) throw std::runtime_error("Cancellation was not preserved");
        cancel=false;
        auto limited=prepared;limited.output_limit=1;
        if(engine.generate(limited,[](std::string_view){}).reason!=opt::FinishReason::OutputLimit)
            throw std::runtime_error("Output limit was treated as complete");
        std::string long_source;
        while (long_source.size() < 6000)
            long_source += "The sample was collected after each experiment. ";
        auto multi_batch = engine.prepare(long_source, opt::Form::Plain);
        if (multi_batch.tokens.size() <= engine.runtime().batch)
            throw std::runtime_error("Long prompt did not cross a decode batch");
        multi_batch.output_limit = 1;
        auto first = engine.generate(multi_batch, [](std::string_view){});
        if (first.reason != opt::FinishReason::OutputLimit || first.output_tokens != 1)
            throw std::runtime_error("Missing final prompt logits after multiple decode batches");
        multi_batch.output_limit = 128;
        auto interrupted = engine.generate(multi_batch, [&](std::string_view){ cancel = true; });
        if (interrupted.reason != opt::FinishReason::Cancelled || !interrupted.output_tokens)
            throw std::runtime_error("Streaming cancellation did not interrupt an active sequence");
        cancel = false;
        auto after=engine.generate(prepared,[](std::string_view){});
        if(after.reason!=opt::FinishReason::Completed || after.text!=baseline.text)
            throw std::runtime_error("Context state leaked after cancellation or output limit");
        prepared.tokens.resize(engine.capacity());
        if(engine.generate(prepared,[](std::string_view){}).reason!=opt::FinishReason::ContextLimit)
            throw std::runtime_error("Context overflow was not rejected");
        std::cout << "cancel_output_limit_context_reset=passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
