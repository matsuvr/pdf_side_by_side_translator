#include "app/bundled_files.hpp"
#include "platform/executable_path.hpp"
#include <iostream>

// Run against a live App Translocation executable and its original ZIP folder.
int main(int argc,char** argv){
    if(argc!=3){std::cerr<<"usage: macos_companion_probe executable expected-distribution-folder\n";return 2;}
    const auto original=opt::original_executable_path(argv[1]);
    const auto directory=opt::distribution_directory(original,opt::ExecutableLayout::MacAppBundle);
    const auto model=opt::bundled_model(directory);
    std::cout<<"original executable: "<<original<<"\ndistribution folder: "<<directory<<'\n';
    if(directory!=std::filesystem::path(argv[2])||!model){std::cerr<<"Companion GGUF was not found beside the original app\n";return 1;}
    std::cout<<"companion GGUF: "<<*model<<'\n';
}
