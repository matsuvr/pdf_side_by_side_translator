#include "ocr/image_reading.hpp"
#include <windows.h>
#include <string_view>
namespace opt {
bool read_bundled_image_model(const char* name,std::vector<char>* data){
    std::string_view file(name);
    if(const auto slash=file.find_last_of("/\\");slash!=file.npos)file.remove_prefix(slash+1);
    const int id=file=="eng.traineddata"?211:file=="chi_sim.traineddata"?212:file=="chi_tra.traineddata"?213:0;
    if(!id)return false; // Tesseract must not fall back to the filesystem.
    const auto module=GetModuleHandleW(nullptr);
    const auto res=FindResourceW(module,MAKEINTRESOURCEW(id),MAKEINTRESOURCEW(10));
    if(!res)return false;
    const auto size=SizeofResource(module,res);
    const auto handle=LoadResource(module,res);
    const auto* ptr=static_cast<const char*>(LockResource(handle));
    if(!ptr||!size)return false;
    data->assign(ptr,ptr+size);
    return true;
}
}
