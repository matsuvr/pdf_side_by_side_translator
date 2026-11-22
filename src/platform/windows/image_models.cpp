#include "ocr/image_reading.hpp"
#include "image_reading_assets.hpp"
#include <windows.h>
#include <algorithm>
#include <string>

namespace opt {
bool read_bundled_image_model(const char* name,std::vector<char>* data){
    std::string path(name);
    std::replace(path.begin(),path.end(),'\\','/');
    std::string_view key(path);
    if(key.starts_with("./"))key.remove_prefix(2);
    constexpr std::string_view suffix=".traineddata";
    if(!key.ends_with(suffix))return false;
    key.remove_suffix(suffix.size());
    for(const auto& model:image_models)if(key==model.key){
        const auto module=GetModuleHandleW(nullptr);
        const auto res=FindResourceW(module,MAKEINTRESOURCEW(model.resource),MAKEINTRESOURCEW(10));
        if(!res)return false;
        const auto size=SizeofResource(module,res);
        const auto handle=LoadResource(module,res);
        const auto* ptr=static_cast<const char*>(LockResource(handle));
        if(!ptr||!size)return false;
        data->assign(ptr,ptr+size);
        return true;
    }
    return false; // No filesystem/network fallback.
}
}
