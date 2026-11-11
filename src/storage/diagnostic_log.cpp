#include "storage/diagnostic_log.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
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
}
