#pragma once
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace opt {
using ImageModelReader = bool (*)(const char*,std::vector<char>*);
inline constexpr int image_reading_dpi = 220;
inline constexpr int image_reading_max_side = 4096;
inline constexpr int image_reading_max_area = 8*1024*1024;
// An empty tag requests language detection. When classification is uncertain,
// text can still be useful; never label the result as a specific language.
struct ImageReadingResult {std::string text,label;bool language_uncertain=false;};
ImageReadingResult read_page_image(const unsigned char* pixels,int width,int height,int stride,int dpi,
                                   std::string_view language_tag,std::stop_token stop,ImageModelReader models);
}
