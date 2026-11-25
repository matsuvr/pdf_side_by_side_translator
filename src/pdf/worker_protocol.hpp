#pragma once
#include "pdf/pdf_service.hpp"
#include <span>
#include <vector>

namespace opt::pdf_wire {
inline constexpr std::size_t max_frame=48U*1024*1024;
inline constexpr std::size_t max_command=64U*1024;
enum class Operation : std::uint32_t { Open, Page, View, Image, Extract, Search, Cancel };
struct Command {
    Operation op=Operation::Open;
    DocumentEpoch document=0;
    PageIndex page=0;
    std::string password;
    ViewRequest view;
    int rotation=0;
    std::uint64_t generation=0;
    PdfSearchRequest search;
    ~Command();
};
using Bytes=std::vector<unsigned char>;
Bytes encode(const Command&);
Command decode_command(std::span<const unsigned char>);
Bytes encode(const PdfEvent&);
PdfEvent decode_event(std::span<const unsigned char>);
}
