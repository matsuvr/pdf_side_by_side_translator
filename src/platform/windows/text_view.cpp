#include "platform/text_view.hpp"
#include "platform/text_format.hpp"
#include <wx/textctrl.h>
#include <wx/fontutil.h>
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
class ScopedReaderUpdate {
    HWND handle_;
public:
    explicit ScopedReaderUpdate(HWND handle):handle_(handle){SendMessageW(handle_,EM_SETREADONLY,FALSE,0);}
    ~ScopedReaderUpdate(){SendMessageW(handle_,EM_SETREADONLY,TRUE,0);}
};
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

Microsoft::WRL::ComPtr<ITextDocument> text_document(HWND handle){
    Microsoft::WRL::ComPtr<IRichEditOle> ole;
    Microsoft::WRL::ComPtr<ITextDocument> document;
    if(SendMessageW(handle,EM_GETOLEINTERFACE,0,reinterpret_cast<LPARAM>(ole.GetAddressOf())))
        ole.As(&document);
    return document;
}
bool east_asian_repertoire(long rep){
    return rep==tomShiftJIS||rep==tomGB2312||rep==tomBIG5||rep==tomHangul;
}
void format_range(ITextDocument& document,LONG start,LONG end,const wxFont& face,LCID locale){
    const wxBasicString name(face.GetFaceName());
    const auto charset=face.GetNativeFontInfo()->lf.lfCharSet;
    long repertoire=tomDefaultCharRep;
    switch(charset){
    case SHIFTJIS_CHARSET:repertoire=tomShiftJIS;break;
    case GB2312_CHARSET:repertoire=tomGB2312;break;
    case CHINESEBIG5_CHARSET:repertoire=tomBIG5;break;
    case HANGUL_CHARSET:repertoire=tomHangul;break;
    }
    Microsoft::WRL::ComPtr<ITextRange> range;
    if(start>=end||!name||FAILED(document.Range(start,start,&range)))return;
    long freeze_count=0;
    const bool frozen=start==0 && SUCCEEDED(document.Freeze(&freeze_count));
    // Rich Edit can assign different writing systems within a single insertion.
    // Change the CJK repertoire on its own runs and retain native shaping for
    // other scripts, emoji and symbols.
    while(start<end){
        long next=start;
        if(FAILED(range->MoveEnd(tomCharFormat,1,nullptr))||FAILED(range->GetEnd(&next))||next<=start)break;
        next=std::min(next,end);
        if(FAILED(range->SetRange(start,next)))break;
        Microsoft::WRL::ComPtr<ITextFont> font;
        if(FAILED(range->GetFont(&font)))break;
        Microsoft::WRL::ComPtr<ITextFont2> extended;
        long current=tomDefaultCharRep;
        if(SUCCEEDED(font.As(&extended)))extended->GetCharRep(&current);
        const bool cjk=east_asian_repertoire(current) && (current!=tomHangul||repertoire==tomHangul);
        if(repertoire==tomDefaultCharRep||cjk||current==tomDefaultCharRep||current==tomAnsi){
            if(cjk && repertoire!=tomDefaultCharRep && repertoire!=current)extended->SetCharRep(repertoire);
            long old_locale=0;
            if(FAILED(font->GetLanguageID(&old_locale))||old_locale!=static_cast<LONG>(locale))
                font->SetLanguageID(locale);
            wxBasicString old_name;
            if(FAILED(font->GetName(old_name.ByRef()))||!old_name||wcscmp(old_name,name)!=0)
                font->SetName(name);
        }
        start=next;
        if(FAILED(range->SetRange(start,start)))break;
    }
    if(frozen)document.Unfreeze(&freeze_count);
}
bool replace_range(HWND handle, LONG start, const wxString& suffix,const wxFont& face,LCID locale) {
    auto document=text_document(handle);
    Microsoft::WRL::ComPtr<ITextRange> tail;
    if (!document || FAILED(document->Range(start, tomForward, &tail))) {
        return false;
    }
    const wxBasicString replacement(suffix);
    if (!replacement) return false;
    const auto result = tail->SetText(replacement);
    if(SUCCEEDED(result)&&!suffix.empty())
        format_range(*document.Get(),start,start+static_cast<LONG>(suffix.size()),face,locale);
    return SUCCEEDED(result);
}
LCID reader_locale(std::string_view language){
    if(language.empty())return LOCALE_INVARIANT;
    const auto locale=LocaleNameToLCID(wxString::FromUTF8(language.data(),language.size()).wc_str(),0);
    return locale?locale:LOCALE_INVARIANT;
}
wxFont reader_font(int points,bool bundled_font,LCID locale){
    const auto lang=LANGIDFROMLCID(locale);
    wxFontInfo font(points);
    switch(PRIMARYLANGID(lang)){
    case LANG_JAPANESE:
        // Memory fonts are selectable by name but Windows does not enumerate
        // them. wxFont::SetFaceName would reject our privately registered face.
        font.FaceName(bundled_font?"Noto Sans CJK JP":"Yu Gothic").Encoding(wxFONTENCODING_CP932);
        break;
    case LANG_CHINESE:
        if(SUBLANGID(lang)==SUBLANG_CHINESE_TRADITIONAL || SUBLANGID(lang)==SUBLANG_CHINESE_HONGKONG ||
           SUBLANGID(lang)==SUBLANG_CHINESE_MACAU)
            font.FaceName("Microsoft JhengHei").Encoding(wxFONTENCODING_CP950);
        else
            font.FaceName("Microsoft YaHei").Encoding(wxFONTENCODING_CP936);
        break;
    case LANG_KOREAN:
        font.FaceName("Malgun Gothic").Encoding(wxFONTENCODING_CP949);
        break;
    default:
        font.FaceName("Segoe UI");
    }
    return wxFont(font);
}
CHARFORMAT2W reader_format(const wxFont& face,LCID locale){
    CHARFORMAT2W format{sizeof(format)};
    format.dwMask=CFM_FACE|CFM_SIZE|CFM_CHARSET|CFM_LCID;
    format.lcid=locale;
    format.yHeight=face.GetPointSize()*20;
    const auto& lf=face.GetNativeFontInfo()->lf;
    format.bCharSet=lf.lfCharSet;
    format.bPitchAndFamily=lf.lfPitchAndFamily;
    wxStrlcpy(format.szFaceName,lf.lfFaceName,LF_FACESIZE);
    return format;
}
}
void set_reader_font(wxTextCtrl& control,int points,bool bundled_font,std::string_view language){
    const auto locale=reader_locale(language);
    const auto face=reader_font(points,bundled_font,locale);
    const auto handle=static_cast<HWND>(control.GetHandle());
    ScopedReaderUpdate guard(handle);
    // Generated text and its formatting must never enter the user's undo stack.
    SendMessageW(handle,EM_SETUNDOLIMIT,0,0);
    control.SetFont(face);
    const auto options=SendMessageW(handle,EM_GETLANGOPTIONS,0,0);
    SendMessageW(handle,EM_SETLANGOPTIONS,0,(options & ~(IMF_AUTOFONT|IMF_DUALFONT)) | IMF_NOIMPLICITLANG);
    auto format=reader_format(face,locale);
    SendMessageW(handle,EM_SETCHARFORMAT,0,reinterpret_cast<LPARAM>(&format));
    if(auto document=text_document(handle))
        format_range(*document.Get(),0,control.GetLastPosition(),face,locale);
}
void style_reader_quotes(wxTextCtrl& control,std::span<const ReaderQuote> quotes,std::string_view language){
    if(quotes.empty())return;
    const auto locale=reader_locale(language);
    const auto face=reader_font(control.GetFont().GetPointSize(),true,locale);
    const auto handle=static_cast<HWND>(control.GetHandle());
    auto document=text_document(handle);
    if(!document)return;
    ScopedReaderUpdate guard(handle);
    for(const auto& quote:quotes)
        format_range(*document.Get(),static_cast<LONG>(quote.begin),static_cast<LONG>(quote.end),face,locale);
}
bool update_readonly_text(wxTextCtrl& control, const std::string& before, const std::string& after, bool reset,
                          std::string_view language) {
    LONG start = 0;
    wxString suffix;
    bool appended = false;
    const auto tail = reset ? std::string_view{} : appended_utf8_suffix(before, after);
    // A CRLF across the boundary must be canonicalized with the prefix.
    if (!tail.empty() && (before.back() != '\r' || tail.front() != '\n')) {
        if (const auto units = utf16_units(before, true)) {
            start = static_cast<LONG>(*units);
            suffix = wxString::FromUTF8(rich_edit_text(tail));
            appended = true;
        }
    }
    if (!appended) {
        const auto old_display = rich_edit_text(before);
        const auto new_display = rich_edit_text(after);
        std::size_t common = 0;
        if (!reset) {
            if (old_display == new_display) return !tail.empty();
            while (common < std::min(old_display.size(), new_display.size()) && old_display[common] == new_display[common])
                ++common;
            while (common < new_display.size() && common > 0 && (static_cast<unsigned char>(new_display[common]) & 0xc0) == 0x80)
                --common;
        }
        start = static_cast<LONG>(wxString::FromUTF8(new_display.data(), common).length());
        suffix = wxString::FromUTF8(new_display.data() + common, new_display.size() - common);
    }
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
        ScopedReaderUpdate guard(handle);
        const auto locale=reader_locale(language);
        const auto face=control.GetFont();
        if (!replace_range(handle,start,suffix,face,locale)) {
            CHARRANGE tail{start, -1};
            SendMessageW(handle, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&tail));
            SendMessageW(handle, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(suffix.wc_str()));
            if(!suffix.empty()){
                tail.cpMax=start+static_cast<LONG>(suffix.size());
                SendMessageW(handle,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&tail));
                auto format=reader_format(face,locale);
                SendMessageW(handle,EM_SETCHARFORMAT,SCF_SELECTION|SCF_CHARREPFROMLCID,reinterpret_cast<LPARAM>(&format));
            }
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
    return !tail.empty();
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
