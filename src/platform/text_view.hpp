#pragma once
#include <string>
class wxTextCtrl;
namespace opt {
// GUI thread only. Edits the changed suffix without stealing focus, preserving
// native UTF-16 selection and pixel scroll position during streaming.
void update_readonly_text(wxTextCtrl& control,const std::string& before,const std::string& after,bool reset);
std::size_t text_logical_anchor(wxTextCtrl& control);
void restore_text_logical_anchor(wxTextCtrl& control,std::size_t utf8_offset);
}
