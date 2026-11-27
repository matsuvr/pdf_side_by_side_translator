#include "ocr/image_reading.hpp"
#include "image_reading_assets.hpp"
#include "core/utf8.hpp"
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
void set_image(tesseract::TessBaseAPI& engine,const GrayPage& page){
    engine.SetImage(page.pixels.data(),page.width,page.height,1,page.stride);
    engine.SetSourceResolution(page.dpi);
}
std::string recognize(const GrayPage& page,std::string model,ImageModelReader models,Clock::time_point deadline){
    tesseract::TessBaseAPI engine;
    if(engine.Init(".",0,model.c_str(),tesseract::OEM_LSTM_ONLY,nullptr,0,nullptr,nullptr,false,models)!=0)
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
    engine.SetPageSegMode(tesseract::PSM_AUTO);set_image(engine,page);
    tesseract::ETEXT_DESC monitor;
    const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
    if(remaining<=0)throw ImageReadingTimeout{};
    monitor.set_deadline_msecs(static_cast<int>(remaining));
    const auto result=engine.Recognize(&monitor);
    if(result!=0)throw std::runtime_error("画像の読み取りが完了しませんでした。");
    std::unique_ptr<char[]> text(engine.GetUTF8Text());
    if(!text)return {};
    const auto size=std::char_traits<char>::length(text.get());
    if(size>image_reading_max_text)throw std::runtime_error("読み取った文字が多すぎます。");
    return std::string(text.get(),size);
}
std::string exact_model(const ImageLanguage& lang){
    std::string model(lang.model);
    if(lang.scripts=="Han")model+="+eng";
    return model;
}
std::string page_script(const GrayPage& page,ImageModelReader models){
    tesseract::TessBaseAPI osd;
    if(osd.Init(".",0,"osd",tesseract::OEM_TESSERACT_ONLY,nullptr,0,nullptr,nullptr,false,models)!=0)return {};
    set_image(osd,page);
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
const ImageLanguage* detector_language(CLD2::Language language,std::string_view script){
    std::string_view tag=CLD2::LanguageCode(language);
    if(tag=="zh"||tag=="zh-Hans"||tag=="zh-CN")tag="zh-hans";
    if(tag=="zh-Hant"||tag=="zh-TW")tag="zh-hant";
    if(tag=="jw")tag="jv";
    if(tag=="iw")tag="he";
    if(tag=="in")tag="id";
    const auto* lang=by_tag(tag);
    return lang&&(script.empty()||matches_script(lang->scripts,script))?lang:nullptr;
}
struct Detection {
    const ImageLanguage* language=nullptr;
    std::array<const ImageLanguage*,3> ranked{};
};
Detection detect_languages(std::string_view text,std::string_view script){
    Detection result;
    if(text.size()<64)return result;
    const CLD2::CLDHints hints{nullptr,nullptr,0,CLD2::UNKNOWN_LANGUAGE};
    CLD2::Language guesses[3]{};
    int shares[3]{},count=0,valid=0;double scores[3]{};bool reliable=false;
    const auto guess=CLD2::ExtDetectLanguageSummaryCheckUTF8(text.data(),static_cast<int>(text.size()),
        true,&hints,0,guesses,shares,scores,nullptr,&count,&reliable,&valid);
    for(std::size_t i=0;i<result.ranked.size();++i)
        if(shares[i]>0)result.ranked[i]=detector_language(guesses[i],script);
    if(reliable&&shares[0]>=75&&count>=40)result.language=detector_language(guess,script);
    return result;
}
ImageReadingResult reading_result(std::string text,const ImageLanguage* language,
                                  const ImageScriptReader* reader,const Detection& detected={}){
    ImageReadingResult result{std::move(text),language,reader};
    auto next=result.alternatives.begin();
    const auto add=[&](const ImageLanguage* candidate){
        if(candidate&&candidate!=language&&next!=result.alternatives.end()&&
           std::find(result.alternatives.begin(),next,candidate)==next)*next++=candidate;
    };
    for(const auto* candidate:detected.ranked)add(candidate);
    const auto scripts=reader?reader->script:language->scripts;
    const auto split=scripts.find('+');
    const auto same_script=[&](const ImageLanguage& candidate){
        return matches_script(candidate.scripts,scripts.substr(0,split))||
               (split!=scripts.npos&&matches_script(candidate.scripts,scripts.substr(split+1)));
    };
    // Prefer related languages before filling remaining slots in catalog order.
    static constexpr std::string_view families[][5]{
        {"de","en","nl"}, {"fr","it","es","ca"}, {"be","ru","bg","sr"},
        {"sk","pl","hr","sl","sr"}, {"lt","lv"}, {"ms","id","jv","tl","ceb"}, {"az","kk","uz"}
    };
    if(language)for(const auto& family:families){
        if(std::find(std::begin(family),std::end(family),language->tag)==std::end(family))continue;
        for(const auto tag:family){
            if(const auto* candidate=by_tag(tag);candidate&&same_script(*candidate))add(candidate);
        }
    }
    for(const auto& candidate:image_languages)if(same_script(candidate))add(&candidate);
    return result;
}
}

static bool contains_replacement(std::string_view text){
    return text.find("\xef\xbf\xbd")!=text.npos;
}
std::optional<std::string_view> replacement_ocr_language(std::string_view native_text){
    if(!contains_replacement(native_text))return std::nullopt;
    // A PDF may encode U+FFFD even in /ActualText. Use the surviving text as
    // a language hint; the missing characters must come from the page image.
    const auto sample=native_text.substr(0,64*1024);
    const auto detected=detect_languages(sample.substr(0,complete_utf8_prefix(sample)),{});
    return detected.language?detected.language->tag:std::string_view{};
}

std::string_view image_source_error(const ImageReadingResult& result){
    if(!result.language)return "言語の自動判定に失敗。言語を選択すれば翻訳できる可能性があります。";
    if(contains_replacement(result.text))
        return "ページ画像から欠落した文字を読み取れませんでした。";
    if(!has_usable_text(result.text))return "ページ画像から文字を見つけられませんでした。";
    return {};
}

void GrayPage::validate() const {
    if(width<=0||height<=0||width>image_reading_max_side||height>image_reading_max_side||
       width*height>image_reading_max_area||stride<width||dpi<=0||
       static_cast<std::size_t>(stride)>pixels.size()/static_cast<std::size_t>(height))
        throw std::invalid_argument("画像の寸法が上限を超えています。");
}

ImageReadingResult recognize_page_image(GrayPage image,std::string_view language_tag,
                                        ImageModelReader models,Clock::time_point limit){
    image.validate();
    if(!language_tag.empty()){
        const auto* lang=by_tag(language_tag);
        if(!lang)throw std::invalid_argument("画像の読み取りに対応していない言語です。");
        return reading_result(recognize(image,exact_model(*lang),models,limit),lang,nullptr);
    }

    const auto script=page_script(image,models);
    const ImageLanguage* only=nullptr;
    for(auto& lang:image_languages)if(matches_script(lang.scripts,script)){
        if(only){only=nullptr;break;}
        only=&lang;
    }
    if(only)return reading_result(recognize(image,exact_model(*only),models,limit),only,nullptr);

    // At most one shared-script pass and one targeted pass, loaded sequentially.
    // A weak/unsupported script result falls back to Latin *without* claiming
    // a precise language, so short or mixed pages can be retried manually.
    const auto* reader=&image_script_readers[0];
    for(const auto& available:image_script_readers)if(available.script==script)reader=&available;
    auto text=recognize(image,std::string(reader->model),models,limit);
    const auto detected=detect_languages(text,reader->script);
    if(const auto* lang=detected.language;lang&&script==reader->script)
        return reading_result(recognize(image,exact_model(*lang),models,limit),lang,nullptr,detected);
    return reading_result(std::move(text),nullptr,reader,detected);
}
}
