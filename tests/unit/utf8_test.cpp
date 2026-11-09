#include "core/utf8.hpp"
#include <iostream>
#include <stdexcept>

void check(bool value) { if (!value) throw std::runtime_error("UTF-8 assertion failed"); }
int main() {
    try {
        opt::Utf8Stream s;
        check(s.push("abc\xe6")=="abc");
        check(!s.complete());
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
        std::cout << "UTF-8 split tokens, overlong, surrogate and out-of-range checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
