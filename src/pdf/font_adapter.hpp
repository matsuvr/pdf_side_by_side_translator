#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>

namespace opt {
struct FontAdapterStats {std::uint64_t mapped=0,full_reads=0,table_reads=0;bool available=false;};
// PDF worker only, between library initialization and destruction. Uses only
// the public FPDF_SYSFONTINFO interface and delegates other fonts to the OS.
class PdfFontAdapter {
public:
    explicit PdfFontAdapter(const std::filesystem::path& resource_directory);
    ~PdfFontAdapter();
    FontAdapterStats stats()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
