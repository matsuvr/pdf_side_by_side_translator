#pragma once
#include <filesystem>
#include <wx/string.h>
namespace opt {
inline std::filesystem::path native_path(const wxString& value){
    const auto bytes=value.ToUTF8();
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(bytes.data()),bytes.length()));
}
inline wxString display_path(const std::filesystem::path& path){
    const auto bytes=path.u8string();
    return wxString::FromUTF8(reinterpret_cast<const char*>(bytes.data()),bytes.size());
}
}
