// Development oracle for inference_probe's prompt-vector fixture format.
#include "llama.h"
#include <charconv>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::vector<llama_token> tokenize(const llama_vocab* vocab,const std::string& text,bool bos,bool special){
    std::vector<llama_token> ids(text.size()+2);
    const int n=llama_tokenize(vocab,text.data(),static_cast<int>(text.size()),ids.data(),
                               static_cast<int>(ids.size()),bos,special);
    if(n<0)throw std::runtime_error("Unable to tokenize fixture");
    ids.resize(n);
    return ids;
}
}

int main(int argc,char** argv){
    const bool rendered=argc==4&&std::string_view(argv[3])=="--rendered";
    if(argc!=3&&!rendered){std::cerr<<"usage: gguf_tokenizer_reference model.gguf samples.tsv [--rendered]\n";return 2;}
    try{
        auto params=llama_model_default_params();
        params.vocab_only=true;
        std::unique_ptr<llama_model,decltype(&llama_model_free)> model(
            llama_model_load_from_file(argv[1],params),llama_model_free);
        if(!model)throw std::runtime_error("Could not load GGUF vocabulary");
        const auto* vocab=llama_model_get_vocab(model.get());
        std::ifstream samples(argv[2]);
        if(!samples)throw std::runtime_error("Could not open samples");
        std::string line;
        while(std::getline(samples,line)){
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            if(line.empty()||line.front()=='#')continue;
            const auto tab=line.find('\t');
            if(tab==line.npos)throw std::runtime_error("Invalid sample");
            const auto name=line.substr(0,tab),hex=line.substr(tab+1);
            if(hex.empty()||hex.size()%2)throw std::runtime_error("Invalid source hex: "+name);
            std::string text;
            for(std::size_t i=0;i<hex.size();i+=2){
                unsigned byte=0;
                const auto end=hex.data()+i+2;
                const auto parsed=std::from_chars(end-2,end,byte,16);
                if(parsed.ec!=std::errc{}||parsed.ptr!=end)throw std::runtime_error("Invalid source hex: "+name);
                text+=static_cast<char>(byte);
            }
            if(rendered){
                // PFN's rendered prompt already contains the initial BOS spelling.
                std::cout<<name<<'\t';
                for(const auto id:tokenize(vocab,text,false,true))std::cout<<id<<' ';
                std::cout<<'\n';
                continue;
            }
            const auto body=tokenize(vocab,text,false,false);
            std::string decoded(text.size()+1,'\0');
            const int n=llama_detokenize(vocab,body.data(),static_cast<int>(body.size()),
                decoded.data(),static_cast<int>(decoded.size()),false,false);
            if(n<0||std::string_view(decoded.data(),n)!=text)
                throw std::runtime_error("GGUF UTF-8 round trip failed: "+name);
            for(bool easy:{false,true}){
                const std::string prompt="<|plamo:tag|>dataset<|plamo:msg|>translation"
                    "<|plamo:tag|>input<|plamo:msg|>"+text+
                    "<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>"+
                    (easy?"ja-easy":"ja<|plamo:key|>form<|plamo:val|>plain")+"<|plamo:msg|>";
                std::cout<<name<<'\t'<<(easy?"easy-auto":"plain-auto")<<'\t'<<hex<<'\t';
                for(const auto id:tokenize(vocab,prompt,true,true))std::cout<<id<<' ';
                std::cout<<'\n';
            }
        }
        if(samples.bad()||!std::cout)throw std::runtime_error("Fixture I/O failed");
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
