#pragma once
#include "ui/shortcut_policy.hpp"
#include <functional>
#include <memory>
class wxWindow;
namespace opt {
bool input_composing(wxWindow* focused);
// macOS command key equivalents can bypass wx key hooks in native text fields.
// This process-local monitor handles only the declared app shortcuts, before
// menu dispatch; marked text, dialogs and menu tracking stay native.
class NativeShortcutMonitor {
public:
    using Handler=std::function<bool(ShortcutKey,ShortcutContext)>;
    NativeShortcutMonitor(wxWindow& window,Handler handler);
    ~NativeShortcutMonitor();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
