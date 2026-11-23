#pragma once
#include <span>
#include <string>
#include <string_view>
class wxTextCtrl;
namespace opt {
struct ReaderQuote;
// The language applies to the displayed text, independently of the input method.
void set_reader_font(wxTextCtrl& control,int points,bool bundled_font,std::string_view language);
void style_reader_quotes(wxTextCtrl& control,std::span<const ReaderQuote> quotes,std::string_view language);
// GUI thread only. Edits the changed suffix without stealing focus, preserving
// native UTF-16 selection and pixel scroll position during streaming. Returns
// true when the new text extends the previous UTF-8 prefix.
bool update_readonly_text(wxTextCtrl& control,const std::string& before,const std::string& after,bool reset,
                          std::string_view language="ja");
std::size_t text_logical_anchor(wxTextCtrl& control);
void restore_text_logical_anchor(wxTextCtrl& control,std::size_t utf8_offset);
}
