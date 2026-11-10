#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
namespace opt {
enum class DiagnosticCode { SessionStarted,ModelReady,ModelFailed,PdfOpened,PdfFailed,ChunkFinished,StorageWarning,SessionClosing };
// Deliberately contains no free-form string, path, password, source or output.
struct DiagnosticRecord {
    DiagnosticCode code=DiagnosticCode::SessionStarted;
    bool metal=false;
    std::uint32_t context=0,threads=0,batch=0,micro_batch=0;
    std::uint32_t page=0,outcome=0;
    double seconds=0;
};
std::string diagnostic_line(const DiagnosticRecord& record);
void append_diagnostic(const std::filesystem::path& directory,const DiagnosticRecord& record);
inline constexpr std::uintmax_t diagnostic_log_limit=256*1024;
}
