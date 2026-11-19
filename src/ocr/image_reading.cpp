#include "ocr/image_reading.hpp"
#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace opt {
std::string read_page_image(const unsigned char* pixels,int width,int height,int stride,int dpi,
                            ImageLanguage language,std::stop_token stop,ImageModelReader models){
    if(stop.stop_requested())return {};
    if(!pixels||!models||width<=0||height<=0||width>image_reading_max_side||
       height>image_reading_max_side||width*height>image_reading_max_area||stride<width||dpi<=0)
        throw std::invalid_argument("画像の寸法が上限を超えています。");
    const char* lang=language==ImageLanguage::SimplifiedChinese?"chi_sim+eng":
        language==ImageLanguage::TraditionalChinese?"chi_tra+eng":"eng";
    tesseract::TessBaseAPI engine;
    if(engine.Init(".",0,lang,tesseract::OEM_LSTM_ONLY,nullptr,0,nullptr,nullptr,false,models)!=0)
        throw std::runtime_error("画像の読み取りに必要なデータを開けませんでした。");
    std::vector<std::string> loaded;
    engine.GetLoadedLanguagesAsVector(&loaded);
    const auto has=[&](std::string_view v){for(auto& entry:loaded)if(entry==v)return true;return false;};
    if(!has("eng")||(language==ImageLanguage::SimplifiedChinese&&!has("chi_sim"))||
       (language==ImageLanguage::TraditionalChinese&&!has("chi_tra")))
        throw std::runtime_error("選択した言語の読み取りデータが見つかりません。");
    if(stop.stop_requested())return {};
    engine.SetPageSegMode(tesseract::PSM_AUTO);
    engine.SetImage(pixels,width,height,1,stride);
    engine.SetSourceResolution(dpi);
    tesseract::ETEXT_DESC monitor;
    monitor.cancel_this=&stop;
    monitor.cancel=[](void* p,int){return static_cast<std::stop_token*>(p)->stop_requested();};
    monitor.set_deadline_msecs(20000);
    const auto result=engine.Recognize(&monitor);
    if(stop.stop_requested())return {};
    if(result!=0)throw std::runtime_error("画像の読み取りが完了しませんでした。");
    std::unique_ptr<char[]> text(engine.GetUTF8Text());
    if(!text)return {};
    std::string output(text.get());
    if(output.size()>256*1024)throw std::runtime_error("読み取った文字が多すぎます。");
    return output;
}
}
