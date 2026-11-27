#pragma once
#include "platform/access_path.hpp"
#include <optional>

namespace opt {
enum class ExecutableLayout { Standalone, MacAppBundle };
#ifdef _WIN32
inline constexpr auto bundled_model_filename="plamo3-translate-0.8b-x86-s4tile-v7.p4x86";
inline constexpr auto bundled_model_filter="PLaMo S4 (*.p4x86)|*.p4x86|すべてのファイル|*";
#else
inline constexpr auto bundled_model_filename="plamo-3-translate-0.8b-Q4_K_M.gguf";
inline constexpr auto bundled_model_filter="GGUF (*.gguf)|*.gguf|すべてのファイル|*";
#endif

inline std::filesystem::path distribution_directory(const std::filesystem::path& executable,ExecutableLayout layout){
    auto directory=executable.parent_path();
    // The executable is App.app/Contents/MacOS/App; companions sit beside App.app.
    if(layout==ExecutableLayout::MacAppBundle)directory=directory.parent_path().parent_path().parent_path();
    return directory;
}

inline std::optional<std::filesystem::path> bundled_model(const std::filesystem::path& directory){
    auto path=directory/bundled_model_filename;
    std::error_code error;
    if(!std::filesystem::exists(access_path(path),error)&&!error)return std::nullopt;
    // Only absence opens a picker. An inaccessible or invalid bundled model
    // goes through the normal loader so its actual failure is reported.
    return path;
}
}
