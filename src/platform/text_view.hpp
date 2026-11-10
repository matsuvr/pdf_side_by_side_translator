#pragma once
#include <string>
class wxTextCtrl;
namespace opt {
// GUI thread only. Edits the changed suffix without stealing focus, preserving
// native UTF-16 selection and pixel scroll position during streaming.
void update_readonly_text(wxTextCtrl& control,const std::string& before,const std::string& after,bool reset);
}
