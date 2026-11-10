#pragma once
#include <span>
#include <string_view>
namespace opt::resources {
std::string_view help();
std::string_view licenses();
std::span<const unsigned char> icon_png();
}
