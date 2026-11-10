#include "storage/diagnostic_log.hpp"
#include "platform/access_path.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
namespace opt {
std::string diagnostic_line(const DiagnosticRecord& r){
    constexpr std::array codes{"session-started","model-ready","model-failed","pdf-opened","pdf-failed","chunk-finished","storage-warning","session-closing"};
    const auto code=static_cast<unsigned>(r.code);
    const auto seconds=std::isfinite(r.seconds)&&r.seconds>=0&&r.seconds<1e9?r.seconds:0;
    std::ostringstream line;line.imbue(std::locale::classic());
    line<<std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()<<' '
        <<(code<codes.size()?codes[code]:"unknown")<<" backend="<<(r.metal?"Metal":"CPU")
        <<" context="<<r.context<<" threads="<<r.threads<<" batch="<<r.batch<<" ubatch="<<r.micro_batch
        <<" page="<<r.page<<" outcome="<<r.outcome<<" seconds="<<std::fixed<<std::setprecision(3)<<seconds<<'\n';
    return line.str();
}
void append_diagnostic(const std::filesystem::path& directory,const DiagnosticRecord& record){
    std::filesystem::create_directories(access_path(directory));const auto current=directory/"diagnostic.log";
    const auto line=diagnostic_line(record);std::error_code error;
    const auto size=std::filesystem::file_size(access_path(current),error);
    if(!error&&size+line.size()>diagnostic_log_limit){
        const auto older=directory/"diagnostic.log.2",previous=directory/"diagnostic.log.1";
        std::filesystem::remove(access_path(older),error);error.clear();
        if(std::filesystem::exists(access_path(previous)))std::filesystem::rename(access_path(previous),access_path(older));
        std::filesystem::rename(access_path(current),access_path(previous));
    }
    std::ofstream out(access_path(current),std::ios::binary|std::ios::app);out<<line;out.flush();
    if(!out)throw std::runtime_error("Diagnostic log unavailable");
}
}
