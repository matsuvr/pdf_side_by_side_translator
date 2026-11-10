#pragma once
#include <filesystem>
namespace opt {
// Keep displayed/persisted paths in their normal form. The Win32 file boundary
// adds an extended absolute path when needed, without changing OS policy.
inline std::filesystem::path access_path(const std::filesystem::path& path){
#ifdef _WIN32
    if(path.native().starts_with(L"\\\\?\\"))return path;
    std::error_code error;const auto absolute=std::filesystem::absolute(path,error);if(error||absolute.native().size()<248)return path;
    const auto& value=absolute.native();
    return std::filesystem::path(value.starts_with(L"\\\\")?L"\\\\?\\UNC\\"+value.substr(2):L"\\\\?\\"+value);
#else
    return path;
#endif
}
}
