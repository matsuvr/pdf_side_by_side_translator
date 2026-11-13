#pragma once
#include <filesystem>

namespace opt {
// Locate companions beside the original app, including a translocated Mac app.
#ifdef __APPLE__
std::filesystem::path original_executable_path(const std::filesystem::path& executable);
#else
inline std::filesystem::path original_executable_path(const std::filesystem::path& executable){return executable;}
#endif
}
