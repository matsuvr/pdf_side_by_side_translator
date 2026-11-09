// Standalone C API consumer: also built by GN to establish the link closure.
#include "fpdfview.h"
#include "fpdf_text.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: pdf_probe input.pdf [output.ppm]\n";
        return 2;
    }
    // The product uses mapping; the small probe owns a stable byte buffer.
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 3;
    std::vector<char> bytes{std::istreambuf_iterator<char>(input), {}};
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
    auto document = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
    if (!document) {
        std::cerr << "FPDF_LoadMemDocument64 failed: " << FPDF_GetLastError() << '\n';
        FPDF_DestroyLibrary();
        return 4;
    }
    auto page = FPDF_LoadPage(document, 0);
    if (!page) {
        FPDF_CloseDocument(document);
        FPDF_DestroyLibrary();
        return 5;
    }
    auto text = FPDFText_LoadPage(page);
    const int count = text ? FPDFText_CountChars(text) : -1;
    std::string extracted;
    for (int i = 0; i < count; ++i) {
        const auto u = FPDFText_GetUnicode(text, i);
        if (u < 128) extracted += static_cast<char>(u);
    }
    const int width = 612, height = 792;
    std::vector<std::uint8_t> pixels(width * height * 4, 255);
    auto bitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA,
                                      pixels.data(), width * 4);
    if (!bitmap) {
        if (text) FPDFText_ClosePage(text);
        FPDF_ClosePage(page);
        FPDF_CloseDocument(document);
        FPDF_DestroyLibrary();
        return 6;
    }
    FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xffffffff);
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, FPDF_ANNOT);
    std::size_t dark = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4)
        if (pixels[i] < 200 || pixels[i + 1] < 200 || pixels[i + 2] < 200) ++dark;
    if (argc > 2) {
        std::ofstream output(argv[2], std::ios::binary);
        output << "P6\n" << width << ' ' << height << "\n255\n";
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            const char rgb[]{static_cast<char>(pixels[i + 2]),
                             static_cast<char>(pixels[i + 1]),
                             static_cast<char>(pixels[i])};
            output.write(rgb, 3);
        }
        if (!output) return 7;
    }
    std::cout << "pages=" << FPDF_GetPageCount(document) << " chars=" << count
              << " dark_pixels=" << dark << "\n" << extracted << '\n';
    FPDFBitmap_Destroy(bitmap);
    if (text) FPDFText_ClosePage(text);
    FPDF_ClosePage(page);
    FPDF_CloseDocument(document);
    FPDF_DestroyLibrary();
    return count > 0 && dark > 0 && extracted.find("Offline PDF") != std::string::npos ? 0 : 8;
}
