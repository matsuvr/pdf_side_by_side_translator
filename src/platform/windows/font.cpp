#include "platform/font.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace opt {
bool register_private_font(const std::filesystem::path&, std::string& error) {
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(201), RT_RCDATA);
    if (!resource) { error = "Embedded font resource not found"; return false; }
    const auto loaded = LoadResource(module, resource);
    const auto data = LockResource(loaded);
    DWORD fonts = 0;
    if (!data || !AddFontMemResourceEx(data, SizeofResource(module, resource), nullptr, &fonts)) {
        error = "Private font registration failed"; return false;
    }
    return fonts > 0;
}
}
