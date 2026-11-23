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
    const std::string beginning="😀\r\n『直辖";
    opt::ReaderQuoteScanner scanner;
    constexpr std::string_view source="设立直辖市 UNDP";
    require(scanner.scan("😀\r",source).empty(),"stream starts before split CRLF");
    require(scanner.scan(beginning,source).empty(),"unclosed quote spans updates");
    const auto text=beginning+"市』と「日本語訳」（UNDP）";
    const auto quotes=scanner.scan(text,source);
    const auto first=*utf16_units("😀\r\n『"
#ifdef _WIN32
        ,true
#endif
    );
    require(quotes.size()==2&&quotes[0].begin==first&&quotes[0].end==first+3&&
            quotes[1].end-quotes[1].begin==4,"only source-matched quoted text gets source-language ranges");
    require(opt::ReaderQuoteScanner{}.scan("直辖市と『未完","直辖市 未完").empty(),"unmarked or unfinished quotes use Japanese");
    require(scanner.scan(text+"続き",source).empty(),
            "streamed text does not format already closed quotes again");
    scanner={};
    require(scanner.scan("「原語」","原語").size()==1,"replacement restarts native offsets");
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
