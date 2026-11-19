#include "ocr/image_reading.hpp"
#include "image_reading_assets.hpp"
#include <public/compact_lang_det.h>
#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>

namespace opt {
namespace {
using Clock=std::chrono::steady_clock;
const ImageLanguage* by_tag(std::string_view tag){
    for(auto& lang:image_languages)if(lang.tag==tag)return &lang;
    return nullptr;
}
bool matches_script(std::string_view languages_scripts,std::string_view script){
    const auto i=languages_scripts.find('+');
    return script==languages_scripts.substr(0,i)||(i!=languages_scripts.npos&&script==languages_scripts.substr(i+1));
}
struct PageImage {
    const unsigned char* data;int w,h,stride,dpi;ImageModelReader models;
    void set(tesseract::TessBaseAPI& engine)const{
        engine.SetImage(data,w,h,1,stride);engine.SetSourceResolution(dpi);
    }
};
std::string recognize(const PageImage& page,std::string model,std::stop_token stop,Clock::time_point deadline){
    if(stop.stop_requested())return {};
    tesseract::TessBaseAPI engine;
    if(engine.Init(".",0,model.c_str(),tesseract::OEM_LSTM_ONLY,nullptr,0,nullptr,nullptr,false,page.models)!=0)
        throw std::runtime_error("画像の読み取りに必要なデータを開けませんでした。");
    std::vector<std::string> loaded;
    engine.GetLoadedLanguagesAsVector(&loaded);
    for(std::size_t start=0,end;start<model.size();start=end+1){
        end=model.find('+',start);
        const auto required=model.substr(start,end-start);
        if(std::find(loaded.begin(),loaded.end(),required)==loaded.end())
            throw std::runtime_error("選択した言語の読み取りデータが見つかりません。");
        if(end==model.npos)break;
    }
    if(stop.stop_requested())return {};
    engine.SetPageSegMode(tesseract::PSM_AUTO);page.set(engine);
    tesseract::ETEXT_DESC monitor;
    monitor.cancel_this=&stop;
    monitor.cancel=[](void* p,int){return static_cast<std::stop_token*>(p)->stop_requested();};
    const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
    if(remaining<=0)throw std::runtime_error("画像の読み取りが時間上限に達しました。");
    monitor.set_deadline_msecs(static_cast<int>(remaining));
    const auto result=engine.Recognize(&monitor);
    if(stop.stop_requested())return {};
    if(result!=0)throw std::runtime_error("画像の読み取りが完了しませんでした。");
    std::unique_ptr<char[]> text(engine.GetUTF8Text());
    if(!text)return {};
    std::string output(text.get());
    if(output.size()>256*1024)throw std::runtime_error("読み取った文字が多すぎます。");
    return output;
}
std::string exact_model(const ImageLanguage& lang){
    std::string model(lang.model);
    if(lang.scripts=="Han")model+="+eng";
    return model;
}
std::string page_script(const PageImage& page){
    tesseract::TessBaseAPI osd;
    if(osd.Init(".",0,"osd",tesseract::OEM_TESSERACT_ONLY,nullptr,0,nullptr,nullptr,false,page.models)!=0)return {};
    page.set(osd);
    const char* script=nullptr;int rotation=0;float orientation=0,confidence=0;
    if(!osd.DetectOrientationScript(&rotation,&orientation,&script,&confidence)||!script)return {};
    std::string name(script);
    // Han competes with the recognizer's Japanese/Korean pseudo-scripts.
    // A weak Han result only selects a broad recognizer; text detection must
    // corroborate it before we label Simplified or Traditional Chinese.
    if(confidence<(name=="Han"?0.2f:1.0f))return {};
    if(name=="Hangul")return "Korean";
    if(name=="Katakana"||name=="Hiragana")return "Japanese";
    return name;
}
const ImageLanguage* text_language(std::string_view text,std::string_view script){
    if(text.size()<64)return nullptr;
    const CLD2::CLDHints hints{nullptr,nullptr,0,CLD2::UNKNOWN_LANGUAGE};
    CLD2::Language guesses[3]{};
    int shares[3]{},count=0,valid=0;double scores[3]{};bool reliable=false;
    const auto guess=CLD2::ExtDetectLanguageSummaryCheckUTF8(text.data(),static_cast<int>(text.size()),
        true,&hints,0,guesses,shares,scores,nullptr,&count,&reliable,&valid);
    if(!reliable||shares[0]<75||count<40||guess==CLD2::UNKNOWN_LANGUAGE)return nullptr;
    std::string_view tag=CLD2::LanguageCode(guess);
    if(tag=="zh"||tag=="zh-Hans"||tag=="zh-CN")tag="zh-hans";
    if(tag=="zh-Hant"||tag=="zh-TW")tag="zh-hant";
    if(tag=="jw")tag="jv";
    if(tag=="iw")tag="he";
    if(tag=="in")tag="id";
    const auto* lang=by_tag(tag);
    return lang&&matches_script(lang->scripts,script)?lang:nullptr;
}
}

ImageReadingResult read_page_image(const unsigned char* pixels,int width,int height,int stride,int dpi,
                                   std::string_view language_tag,std::stop_token stop,ImageModelReader models){
    if(stop.stop_requested())return {};
    if(!pixels||!models||width<=0||height<=0||width>image_reading_max_side||
       height>image_reading_max_side||width*height>image_reading_max_area||stride<width||dpi<=0)
        throw std::invalid_argument("画像の寸法が上限を超えています。");
    const PageImage image{pixels,width,height,stride,dpi,models};
    const auto limit=Clock::now()+std::chrono::seconds(20);
    if(!language_tag.empty()){
        const auto* lang=by_tag(language_tag);
        if(!lang)throw std::invalid_argument("画像の読み取りに対応していない言語です。");
        return {recognize(image,exact_model(*lang),stop,limit),std::string(lang->label),false};
    }

    const auto script=page_script(image);
    if(stop.stop_requested())return {};
    const ImageLanguage* only=nullptr;
    for(auto& lang:image_languages)if(matches_script(lang.scripts,script)){
        if(only){only=nullptr;break;}
        only=&lang;
    }
    if(only)return {recognize(image,exact_model(*only),stop,limit),std::string(only->label),false};

    // At most one shared-script pass and one targeted pass, loaded sequentially.
    // A weak/unsupported script result falls back to Latin *without* claiming
    // a precise language, so short or mixed pages can be retried manually.
    std::string_view group=script;
    std::string generic;
    if(script=="Han")generic="chi_sim+chi_tra";
    else if(script=="Cyrillic"||script=="Arabic"||script=="Devanagari")generic="script/"+script;
    else{generic="script/Latin";group="Latin";}
    auto text=recognize(image,std::move(generic),stop,limit);
    if(stop.stop_requested())return {};
    if(!script.empty()&&script==group)if(const auto* lang=text_language(text,group)){
        // The shared-script result is still useful if the specific model
        // cannot finish inside the shared time budget.
        try{
            auto refined=recognize(image,exact_model(*lang),stop,limit);
            if(!refined.empty())return {std::move(refined),std::string(lang->label),false};
        }catch(const std::runtime_error&){if(stop.stop_requested())return {};}
    }
    if(stop.stop_requested())return {};
    return {std::move(text),{},true};
}
}
