#include "platform/executable_path.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <memory>
#include <type_traits>
#include <vector>

namespace opt {
std::filesystem::path original_executable_path(const std::filesystem::path& executable){
    // Security exposes this read-only lookup on macOS 10.12 and later, but not
    // in the public SDK headers. Resolve it at runtime and retain the supplied
    // path if it is unavailable. Never alter quarantine or move/copy the app.
    const std::unique_ptr<void,decltype(&dlclose)> security(
        dlopen("/System/Library/Frameworks/Security.framework/Security",RTLD_LAZY|RTLD_LOCAL),dlclose);
    if(!security)return executable;
    using IsTranslocated=Boolean (*)(CFURLRef,bool*,CFErrorRef*);
    using OriginalPath=CFURLRef (*)(CFURLRef,CFErrorRef*);
    const auto is_translocated=reinterpret_cast<IsTranslocated>(dlsym(security.get(),"SecTranslocateIsTranslocatedURL"));
    const auto original_path=reinterpret_cast<OriginalPath>(dlsym(security.get(),"SecTranslocateCreateOriginalPathForURL"));
    if(!is_translocated||!original_path)return executable;
    const auto& bytes=executable.native();
    const auto url=CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(bytes.data()),bytes.size(),false);
    if(!url)return executable;
    bool translocated=false;
    if(!is_translocated(url,&translocated,nullptr)||!translocated){CFRelease(url);return executable;}
    const auto original=original_path(url,nullptr);
    CFRelease(url);
    if(!original)return executable;
    const std::unique_ptr<std::remove_pointer_t<CFStringRef>,decltype(&CFRelease)> path(
        CFURLCopyFileSystemPath(original,kCFURLPOSIXPathStyle),CFRelease);
    CFRelease(original);
    if(!path)return executable;
    const auto capacity=CFStringGetMaximumSizeOfFileSystemRepresentation(path.get());
    std::vector<char> buffer(capacity>0?static_cast<std::size_t>(capacity):1);
    const bool converted=capacity>0&&CFStringGetFileSystemRepresentation(path.get(),buffer.data(),capacity);
    return converted?std::filesystem::path(buffer.data()):executable;
}
}
