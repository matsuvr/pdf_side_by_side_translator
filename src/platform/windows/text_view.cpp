#include "platform/text_view.hpp"
#include "platform/text_format.hpp"
#include <wx/textctrl.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <wrl/client.h>
#include <wx/msw/ole/oleutils.h>
#include <algorithm>

namespace opt {
namespace {
bool at_scroll_bottom(HWND handle) {
    // ES_DISABLENOSCROLL keeps a disabled scrollbar with a dummy range
    // until the wrapped text first outgrows the viewport.
    SCROLLBARINFO bar{sizeof(SCROLLBARINFO)};
    if (GetScrollBarInfo(handle, OBJID_VSCROLL, &bar) && (bar.rgstate[0] & STATE_SYSTEM_UNAVAILABLE)) {
        return true;
    }
    SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS};
    return GetScrollInfo(handle, SB_VERT, &scroll) &&
           scroll.nPos + static_cast<int>(scroll.nPage) >= scroll.nMax - 2;
}

bool replace_range(HWND handle, LONG start, const wxString& suffix) {
    Microsoft::WRL::ComPtr<IRichEditOle> ole;
    Microsoft::WRL::ComPtr<ITextDocument> document;
    Microsoft::WRL::ComPtr<ITextRange> tail;
    if (!SendMessageW(handle, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(ole.GetAddressOf())) ||
        FAILED(ole.As(&document)) ||
        FAILED(document->Range(start, tomForward, &tail))) {
        return false;
    }
    const wxBasicString replacement(suffix);
    if (!replacement) return false;
    // Generated text must not accumulate user undo records.
    const bool suspended = document->Undo(tomSuspend, nullptr) == S_OK;
    const auto result = tail->SetText(replacement);
    if (suspended) document->Undo(tomResume, nullptr);
    return SUCCEEDED(result);
}
}
void update_readonly_text(wxTextCtrl& control, const std::string& before, const std::string& after, bool reset) {
    const auto old_display = rich_edit_text(before);
    const auto new_display = rich_edit_text(after);
    std::size_t common = 0;
    if (!reset) {
        if (old_display == new_display) return;
        while (common < std::min(old_display.size(), new_display.size()) && old_display[common] == new_display[common])
            ++common;
        while (common < new_display.size() && common > 0 && (static_cast<unsigned char>(new_display[common]) & 0xc0) == 0x80)
            --common;
    }
    const auto start = static_cast<LONG>(wxString::FromUTF8(new_display.data(), common).length());
    const auto suffix = wxString::FromUTF8(new_display.data() + common, new_display.size() - common);
    const auto handle = static_cast<HWND>(control.GetHandle());
    CHARRANGE selection{};
    POINT position{};
    SendMessageW(handle, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    const bool follow = !reset && selection.cpMin == selection.cpMax && at_scroll_bottom(handle);
    if (!reset && !follow)
        SendMessageW(handle, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&position));

    // Editing a detached range lets Rich Edit invalidate the reflowed lines
    // without moving the reader's selection or repainting the entire pane.
    {
        struct RestoreReadOnly {
            HWND handle;
            ~RestoreReadOnly() { SendMessageW(handle, EM_SETREADONLY, TRUE, 0); }
        } guard{handle};
        SendMessageW(handle, EM_SETREADONLY, FALSE, 0);
        if (!replace_range(handle, start, suffix)) {
            CHARRANGE tail{start, -1};
            SendMessageW(handle, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&tail));
            SendMessageW(handle, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(suffix.wc_str()));
        }
    }

    const auto length = start + static_cast<LONG>(suffix.length());
    selection.cpMin = reset ? 0 : std::min(selection.cpMin, length);
    selection.cpMax = reset ? 0 : std::min(selection.cpMax, length);
    CHARRANGE current{};
    SendMessageW(handle, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&current));
    if (current.cpMin != selection.cpMin || current.cpMax != selection.cpMax)
        SendMessageW(handle, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));

    if (follow) {
        if (!at_scroll_bottom(handle)) SendMessageW(handle, WM_VSCROLL, SB_BOTTOM, 0);
    } else {
        POINT current_position{};
        SendMessageW(handle, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&current_position));
        if (current_position.x != position.x || current_position.y != position.y)
            SendMessageW(handle, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&position));
    }
}
std::size_t text_logical_anchor(wxTextCtrl& control){
    const auto handle=(HWND)control.GetHandle();POINTL point{1,1};
    const auto at=std::clamp<LRESULT>(SendMessageW(handle,EM_CHARFROMPOS,0,(LPARAM)&point),0,control.GetLastPosition());
    return control.GetRange(0,static_cast<long>(at)).ToUTF8().length();
}
void restore_text_logical_anchor(wxTextCtrl& control,std::size_t offset){
    const auto value=control.GetValue();const auto bytes=value.ToUTF8();offset=std::min(offset,bytes.length());
    while(offset>0&&offset<bytes.length()&&(static_cast<unsigned char>(bytes.data()[offset])&0xc0)==0x80)--offset;
    const auto at=wxString::FromUTF8(bytes.data(),offset).ToStdWstring().size();const auto handle=(HWND)control.GetHandle();
    const auto target=SendMessageW(handle,EM_EXLINEFROMCHAR,0,static_cast<LPARAM>(at));
    const auto first=SendMessageW(handle,EM_GETFIRSTVISIBLELINE,0,0);
    SendMessageW(handle,EM_LINESCROLL,0,target-first);
}
}
