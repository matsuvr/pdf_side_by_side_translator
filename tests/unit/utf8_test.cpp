#include "core/utf8.hpp"
#include "inference/utf8_token_filter.hpp"
#include "platform/text_format.hpp"
#include <array>
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

void test_utf8_token_selection() {
    // Token 0 represents a stop. A stop or a word may outrank every legal
    // continuation while generating a name one byte at a time.
    constexpr std::array<std::string_view, 12> pieces{
        "", "word", "\xf0", "\x80", "\x90", "\x9f", "\x98\x80!",
        "\xbf", "\xc0", "\xed\xa0", "\x83\x01", "\n"
    };
    opt::Utf8TokenFilter filter;
    for (std::size_t i = 1; i < pieces.size(); ++i)
        filter.add(static_cast<std::int32_t>(i), pieces[i]);
    const std::array<float, pieces.size()> logits{101, 100, 12, 95, 1, 1, 90, 1, 80, 80, 99, 0};
    auto masked = logits;
    filter.mask_invalid_starters(masked.data());
    const auto excluded = -std::numeric_limits<float>::infinity();
    for (auto id : {3, 4, 5, 6, 7, 8, 9, 10}) check(masked[id] == excluded);
    for (auto id : {0, 1, 2, 11}) check(masked[id] == logits[id]);

    opt::Utf8Stream stream;
    check(stream.push("Romina Ca\xf0").ends_with("Ca"));
    check(filter.best_continuation(logits.data(), stream) == 4); // F0 needs 90..BF
    check(stream.push(pieces[5]).empty());
    auto scores = logits;
    scores[6] = 96;
    check(filter.best_continuation(scores.data(), stream) == 6);
    check(stream.push(pieces[6]) == "😀!" && stream.complete());

    check(stream.push("\xf4").empty()); // F4 needs 80..8F
    check(filter.best_continuation(logits.data(), stream) == 3);
    scores.fill(excluded);
    check(filter.best_continuation(scores.data(), stream) == -1);
    stream.reset();
    check(stream.push("\xed").empty());
    check(!stream.accepts("\xa0") && stream.accepts("\x9f"));
    stream.reset();
    check(stream.push("\xe0").empty());
    check(!stream.accepts("\x9f") && stream.accepts("\xa0"));
}

int main() {
    try {
        test_utf8_token_selection();
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
        check(!opt::has_usable_text("\xee\xb8\x80"));
        check(!opt::has_usable_text("\x01\x7f\xc2\x85"));
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
