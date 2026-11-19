#pragma once
#include <stop_token>
#include <string>
#include <vector>

namespace opt {
enum class ImageLanguage { English, SimplifiedChinese, TraditionalChinese };
using ImageModelReader = bool (*)(const char*,std::vector<char>*);
inline constexpr int image_reading_dpi = 220;
inline constexpr int image_reading_max_side = 4096;
inline constexpr int image_reading_max_area = 8*1024*1024;
std::string read_page_image(const unsigned char* pixels,int width,int height,int stride,int dpi,
                            ImageLanguage language,std::stop_token stop,ImageModelReader models);
}
