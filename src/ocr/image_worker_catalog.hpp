#pragma once
#include "ocr/image_reading.hpp"
#include "image_reading_assets.hpp"
#include <cstdint>
#include <utility>

namespace opt {
template<class T,std::size_t N>
const T* image_entry_at(const T (&catalog)[N],int index){
    if(index==-1)return nullptr;
    if(index<0||static_cast<std::size_t>(index)>=N)throw std::invalid_argument("Invalid OCR catalog index");
    return &catalog[index];
}

inline int image_language_index(std::string_view tag){
    if(tag.empty())return -1;
    for(std::size_t i=0;i<std::size(image_languages);++i)
        if(image_languages[i].tag==tag)return static_cast<int>(i);
    throw std::invalid_argument("Unsupported OCR language");
}

struct ImageWorkerResult {
    int language=-1,script_reader=-1;
    std::array<int,ImageAlternatives{}.size()> alternatives{};
    std::uint32_t text_size=0;

    ImageWorkerResult()=default;
    explicit ImageWorkerResult(const ImageReadingResult& result):
        language(result.language?static_cast<int>(result.language-image_languages):-1),
        script_reader(result.script_reader?static_cast<int>(result.script_reader-image_script_readers):-1),
        text_size(static_cast<std::uint32_t>(result.text.size())){
        for(std::size_t i=0;i<alternatives.size();++i){
            const auto* candidate=result.alternatives[i];
            alternatives[i]=candidate?static_cast<int>(candidate-image_languages):-1;
        }
    }

    ImageReadingResult decode(std::string text)const{
        ImageReadingResult result{std::move(text),image_entry_at(image_languages,language),
            image_entry_at(image_script_readers,script_reader)};
        for(std::size_t i=0;i<alternatives.size();++i)
            result.alternatives[i]=image_entry_at(image_languages,alternatives[i]);
        return result;
    }
};
}
