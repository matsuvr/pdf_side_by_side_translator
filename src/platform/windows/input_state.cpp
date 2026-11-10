#include "platform/input_state.hpp"
#include <wx/window.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <imm.h>
namespace opt {
bool input_composing(wxWindow* focused){
    if(!focused)return false;auto handle=GetFocus();if(!handle)handle=(HWND)focused->GetHandle();
    const auto context=ImmGetContext(handle);if(!context)return false;
    const bool composing=ImmGetCompositionStringW(context,GCS_COMPSTR,nullptr,0)>0;
    ImmReleaseContext(handle,context);return composing;
}
struct NativeShortcutMonitor::Impl {};
NativeShortcutMonitor::NativeShortcutMonitor(wxWindow&,Handler):impl_(std::make_unique<Impl>()){}
NativeShortcutMonitor::~NativeShortcutMonitor()=default;
}
