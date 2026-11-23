#pragma once
#include <chrono>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace opt {
using ImageModelReader = bool (*)(const char*,std::vector<char>*);
inline constexpr int image_reading_dpi = 220;
inline constexpr int image_reading_max_side = 4096;
inline constexpr int image_reading_max_area = 8*1024*1024;
inline constexpr std::size_t image_reading_max_text = 256*1024;
inline constexpr auto image_reading_timeout = std::chrono::seconds(20);
struct ImageReadingTimeout : std::runtime_error {
    ImageReadingTimeout():std::runtime_error("画像の読み取りが時間上限に達しました。"){}
};
struct GrayPage {
    std::span<const unsigned char> pixels;
    int width,height,stride,dpi;
    void validate() const;
};
// An empty tag requests language detection. When classification is uncertain,
// text can still be useful; never label the result as a specific language.
struct ImageReadingResult {std::string text,label;bool language_uncertain=false;std::string_view language_tag;};
ImageReadingResult read_page_image(GrayPage page,std::string_view language_tag,std::stop_token stop,
    std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::now()+image_reading_timeout);

// The viewer and its integration test run the same worker entry point.
inline constexpr char image_worker_switch[]="--internal-ocr-worker";
bool read_bundled_image_model(const char* name,std::vector<char>* data);
int run_image_worker(std::string_view mapping_handle,ImageModelReader models);
ImageReadingResult recognize_page_image(GrayPage image,std::string_view language_tag,
    ImageModelReader models,std::chrono::steady_clock::time_point deadline);
}
