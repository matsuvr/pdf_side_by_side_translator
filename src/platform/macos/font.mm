#include "platform/font.hpp"
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

namespace opt {
bool register_private_font(const std::filesystem::path& directory, std::string& error) {
    @autoreleasepool {
        const auto path = directory / "NotoSansCJKjp-Regular.otf";
        auto url = [NSURL fileURLWithFileSystemRepresentation:path.c_str() isDirectory:NO relativeToURL:nil];
        CFErrorRef failure = nullptr;
        const bool ok = CTFontManagerRegisterFontsForURL((__bridge CFURLRef)url,
                                                        kCTFontManagerScopeProcess, &failure);
        if (!ok && failure) {
            auto message = CFErrorCopyDescription(failure);
            const char* utf8 = [(__bridge NSString*)message UTF8String];
            error = utf8 ? utf8 : "Font registration failed";
            CFRelease(message);
        }
        if (failure) CFRelease(failure);
        return ok;
    }
}
}
