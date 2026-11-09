#pragma once
#include <filesystem>
#include <string>

namespace opt {
// Registers only for this process; never installs a system-wide font.
bool register_private_font(const std::filesystem::path& resource_directory, std::string& error);
}
