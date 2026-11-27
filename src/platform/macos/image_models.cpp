#include "ocr/image_reading.hpp"
#include "image_reading_assets.hpp"

extern "C" const char opt_ocr_models[];

namespace opt {
bool read_bundled_image_model(const char* name,std::vector<char>* data){
    std::string_view key(name);
    if(key.starts_with("./"))key.remove_prefix(2);
    constexpr std::string_view suffix=".traineddata";
    if(!key.ends_with(suffix))return false;
    key.remove_suffix(suffix.size());
    for(const auto& model:image_models)if(key==model.key){
        const auto* start=opt_ocr_models+model.offset;
        data->assign(start,start+model.size);
        return true;
    }
    return false;
}
}
