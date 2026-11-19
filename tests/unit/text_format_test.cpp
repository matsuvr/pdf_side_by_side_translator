#include "platform/text_format.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
}

int main(){try{
    using opt::appended_utf8_suffix;
    using opt::utf16_units;
    require(utf16_units("")==0,"empty UTF-16 length");
    require(utf16_units("日本語 😀")==6,"BMP and supplementary code points");
    require(utf16_units("日\r\n本\n😀\r")==8,"plain line endings");
    require(utf16_units("日\r\n本\n😀\r",true)==7,"Rich Edit paragraph marks");
    require(opt::rich_edit_text("日\r\n本\n😀\r")=="日\r本\r😀\r","Rich Edit display form");
    for(const std::string_view invalid:{"\x80","\xc2","\xc0\xaf","\xe0\x80\x80","\xed\xa0\x80","\xf4\x90\x80\x80"}){
        require(!utf16_units(invalid),"invalid UTF-8 must use the normal conversion path");
        require(!utf16_units(invalid,true),"invalid UTF-8 must use the normal Rich Edit conversion path");
    }
    require(appended_utf8_suffix("😀","😀続き")=="続き","valid append");
    require(appended_utf8_suffix("\xe6","日").empty(),"append splits a scalar");
    require(appended_utf8_suffix("原文","訳文").empty(),"replacement has no suffix");
    require(appended_utf8_suffix("同文","同文").empty(),"unchanged text has no suffix");
    require(appended_utf8_suffix("","先頭").empty(),"initial text uses full conversion");
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
