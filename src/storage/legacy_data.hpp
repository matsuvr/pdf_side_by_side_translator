#pragma once
#include <filesystem>
#include <system_error>

namespace opt {
struct LegacyCleanupResult {
    std::filesystem::path failed_path;
    std::error_code error;
};
// Discard only the files written automatically by the old disk-backed Store.
// Never creates a directory, reads a database, or walks an unknown subtree.
LegacyCleanupResult discard_legacy_data(const std::filesystem::path& directory);
}
