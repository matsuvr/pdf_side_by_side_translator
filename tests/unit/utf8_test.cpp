#include "core/utf8.hpp"
#include "platform/text_format.hpp"
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <new>

namespace {bool count_allocations=false;std::size_t allocations=0;}
void* operator new(std::size_t size){
    if(count_allocations)++allocations;
    if(auto* value=std::malloc(size?size:1))return value;
    throw std::bad_alloc();
}
void operator delete(void* value)noexcept{std::free(value);}
void operator delete(void* value,std::size_t)noexcept{std::free(value);}

void check(bool value) { if (!value) throw std::runtime_error("UTF-8 assertion failed"); }
int main() {
    try {
        opt::Utf8Stream s;
        check(s.push("abc\xe6")=="abc");
        check(!s.complete());
        const std::string large(8*1024*1024,'a');
        allocations=0;count_allocations=true;
        opt::validate_utf8(large);
        const bool usable=opt::has_usable_text(large);
        const bool translation=opt::valid_translation(large);
        count_allocations=false;
        check(usable&&translation&&allocations==0);
        check(!opt::has_usable_text(" \t　� "));
        for(const auto* invalid:{"a\xc0\x80","a\xed\xa0\x80","a\xf4\x90\x80\x80","a\xe6\x97"}){
            bool failed=false;
            try{opt::has_usable_text(invalid);}catch(const opt::Utf8Error&){failed=true;}
            check(failed);
        }
        struct Cancelled{};
        unsigned checkpoints=0;bool cancelled=false;
        try{opt::has_usable_text(large,[&]{if(++checkpoints==3)throw Cancelled{};});}
        catch(const Cancelled&){cancelled=true;}
        check(cancelled&&checkpoints==3);
        check(opt::valid_translation("通常の訳文。\t\r\n続き"));
        check(!opt::valid_translation(std::string("text\0hidden",11)));
        check(!opt::valid_translation("text\x1b[31m"));
        check(!opt::valid_translation("text\x7f"));
        check(opt::rich_edit_text("a\r\nb\nc\rd")=="a\rb\rc\rd");
        check(opt::rich_edit_text("途中\r")==opt::rich_edit_text("途中\r\n"));
        check(s.push("\x97").empty());
        check(s.push("\xa5\xf0\x9f")=="日");
        check(s.push("\x98\x80")=="😀");
        check(s.complete());
        for (const auto* invalid : {"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80", "\xe0\x80"}) {
            s.reset();
            bool failed = false;
            try { s.push(invalid); } catch (const std::runtime_error&) { failed = true; }
            check(failed);
        }
        s.reset();
        check(s.push("\xe6\x97").empty());
        check(!s.complete());
        std::cout << "UTF-8 split tokens, full validation, constant-memory scanning and cancellation passed\n";
    } catch (const std::exception& e) { count_allocations=false;std::cerr << e.what() << '\n'; return 1; }
}
