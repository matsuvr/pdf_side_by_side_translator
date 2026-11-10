#include "platform/text_view.hpp"
#include <wx/textctrl.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <richedit.h>
#include <algorithm>

namespace opt {
void update_readonly_text(wxTextCtrl& control,const std::string& before,const std::string& after,bool reset){
    const auto handle=(HWND)control.GetHandle();
    CHARRANGE selection{};POINT position{};SCROLLINFO scroll{sizeof(SCROLLINFO),SIF_ALL};
    SendMessageW(handle,EM_EXGETSEL,0,(LPARAM)&selection);SendMessageW(handle,EM_GETSCROLLPOS,0,(LPARAM)&position);
    GetScrollInfo(handle,SB_VERT,&scroll);
    const bool follow=!reset&&selection.cpMin==selection.cpMax&&scroll.nPos+static_cast<int>(scroll.nPage)>=scroll.nMax-2;
    std::size_t common=0;
    if(!reset){while(common<std::min(before.size(),after.size())&&before[common]==after[common])++common;
        while(common<after.size()&&common>0&&(static_cast<unsigned char>(after[common])&0xc0)==0x80)--common;}
    const auto prefix=wxString::FromUTF8(after.data(),common).ToStdWstring();
    auto suffix=wxString::FromUTF8(after.data()+common,after.size()-common).ToStdWstring();
    std::replace(suffix.begin(),suffix.end(),L'\n',L'\r');
    CHARRANGE tail{static_cast<LONG>(prefix.size()),-1};
    SendMessageW(handle,WM_SETREDRAW,FALSE,0);SendMessageW(handle,EM_SETREADONLY,FALSE,0);
    SendMessageW(handle,EM_EXSETSEL,0,(LPARAM)&tail);SendMessageW(handle,EM_REPLACESEL,FALSE,(LPARAM)suffix.c_str());
    SendMessageW(handle,EM_SETREADONLY,TRUE,0);
    const auto length=static_cast<LONG>(prefix.size()+suffix.size());
    selection.cpMin=reset?0:std::min(selection.cpMin,length);selection.cpMax=reset?0:std::min(selection.cpMax,length);
    SendMessageW(handle,EM_EXSETSEL,0,(LPARAM)&selection);
    if(follow)SendMessageW(handle,WM_VSCROLL,SB_BOTTOM,0);
    else {if(reset)position={0,0};SendMessageW(handle,EM_SETSCROLLPOS,0,(LPARAM)&position);}
    SendMessageW(handle,WM_SETREDRAW,TRUE,0);InvalidateRect(handle,nullptr,FALSE);
}
std::size_t text_logical_anchor(wxTextCtrl& control){
    const auto handle=(HWND)control.GetHandle();POINTL point{1,1};
    const auto at=std::clamp<LRESULT>(SendMessageW(handle,EM_CHARFROMPOS,0,(LPARAM)&point),0,control.GetLastPosition());
    return control.GetRange(0,static_cast<long>(at)).ToUTF8().length();
}
void restore_text_logical_anchor(wxTextCtrl& control,std::size_t offset){
    const auto bytes=control.GetValue().ToUTF8();offset=std::min(offset,bytes.length());
    while(offset>0&&offset<bytes.length()&&(static_cast<unsigned char>(bytes.data()[offset])&0xc0)==0x80)--offset;
    const auto at=wxString::FromUTF8(bytes.data(),offset).ToStdWstring().size();const auto handle=(HWND)control.GetHandle();
    const auto target=SendMessageW(handle,EM_EXLINEFROMCHAR,0,static_cast<LPARAM>(at));
    const auto first=SendMessageW(handle,EM_GETFIRSTVISIBLELINE,0,0);
    SendMessageW(handle,EM_LINESCROLL,0,target-first);
}
}
