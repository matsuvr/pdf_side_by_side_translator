#include "app/bundled_files.hpp"
#include "platform/digest.hpp"
#include <cassert>
#include <fstream>
#include <iostream>

namespace fs=std::filesystem;

int main(){
    const auto root=fs::temp_directory_path()/("opt-bundle-"+opt::new_uuid());
    const auto distribution=root/fs::path(u8"配布フォルダー with spaces");
    const auto other=root/"working-directory";
    fs::create_directories(distribution);
    fs::create_directories(other);
    struct Cleanup {fs::path root;~Cleanup(){std::error_code ignored;fs::remove_all(root,ignored);}} cleanup{root};

    const auto standalone=distribution/"offline-pdf-translator.exe";
    const auto mac=distribution/"Offline PDF Translator.app"/"Contents"/"MacOS"/"Offline PDF Translator";
    fs::create_directories(mac.parent_path());
    assert(opt::distribution_directory(standalone,opt::ExecutableLayout::Standalone)==distribution);
    assert(opt::distribution_directory(mac,opt::ExecutableLayout::MacAppBundle)==distribution);
    assert(!opt::bundled_model(distribution));

    // Models in the working directory, inside the .app, or under another name
    // must not hide a missing companion in the distribution directory.
    std::ofstream(other/opt::bundled_model_filename)<<"working-directory decoy";
    std::ofstream(mac.parent_path()/opt::bundled_model_filename)<<"inside-bundle decoy";
    std::ofstream(distribution/"other.gguf")<<"other model";
    const auto original_cwd=fs::current_path();
    struct RestoreCwd {fs::path path;~RestoreCwd(){fs::current_path(path);}} restore{original_cwd};
    fs::current_path(other);
    assert(!opt::bundled_model(distribution));

    const auto expected=distribution/opt::bundled_model_filename;
    // Even a damaged/empty file is passed to the loader, not treated as absent.
    std::ofstream(expected).close();
    for(const auto& directory:{opt::distribution_directory(standalone,opt::ExecutableLayout::Standalone),
                               opt::distribution_directory(mac,opt::ExecutableLayout::MacAppBundle)}){
        const auto model=opt::bundled_model(directory);
        assert(model&&*model==expected);
    }
    fs::remove(expected);
    assert(!opt::bundled_model(distribution));
    std::cout<<"bundled model paths and missing-file fallback passed\n";
}
