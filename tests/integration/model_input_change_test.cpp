#include "inference/engine.hpp"
#include "platform/digest.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}

int main(int argc,char** argv){
    if(argc!=2){std::cerr<<"usage: model_input_change_test model.gguf\n";return 2;}
    const auto directory=fs::temp_directory_path()/("opt-model-change-"+opt::new_uuid());
    try{
        fs::create_directory(directory);
        const auto model=directory/"test-model.gguf";
        // Only this owned test copy is modified, never the supplied model.
        fs::copy_file(fs::u8path(argv[1]),model);
        {
            std::atomic_bool cancelled{false};
            opt::Engine engine(cancelled);
            const auto before=std::chrono::steady_clock::now();
            engine.load(model,true);
            std::cout<<"load_seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-before).count()<<std::endl;
            auto input=engine.prepare("The sample was collected after each experiment. The laboratory measured the temperature of the water.",opt::Form::Plain);
            input.output_limit=16;
            bool changed=false;
            const auto result=engine.generate(input,[&](std::string_view text){
                if(!text.empty()&&!changed){
#ifdef _WIN32
                    std::error_code blocked;fs::resize_file(model,0,blocked);
                    require(bool(blocked),"GGUF was writable during generation");
#else
                    fs::resize_file(model,0);
#endif
                    changed=true;
                }
            });
            require(changed,"generation did not reach the model-mutation callback");
#ifdef _WIN32
            require(!result.text.empty()&&(result.reason==opt::FinishReason::OutputLimit||result.reason==opt::FinishReason::Completed),
                    "Generation failed after denying the conflicting write");
            engine.unload();
            fs::resize_file(model,0);
            std::cout<<"model write denied during generation and allowed after unload\n";
#else
            require(result.reason==opt::FinishReason::ModelUnavailable,"changed model did not stop as a normal unavailable-model result");
            require(!result.text.empty(),"the partial result before mutation was lost");
            const auto next=engine.generate(input,[](std::string_view){});
            require(next.reason==opt::FinishReason::ModelUnavailable&&next.text.empty(),"a changed model accepted another generation");
            engine.unload();
            std::cout<<"model truncation during generation returned normally with partial text and rejected reuse\n";
#endif
        }
        fs::remove_all(directory);
        return 0;
    }catch(const std::exception& error){
        std::error_code ignored;fs::remove_all(directory,ignored);
        std::cerr<<error.what()<<'\n';return 1;
    }
}
