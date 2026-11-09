#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace opt {
// Retains an incomplete scalar between token pieces. Rejects invalid UTF-8.
class Utf8Stream {
public:
    std::string push(std::string_view bytes) {
        pending_.append(bytes);
        std::size_t i = 0;
        while (i < pending_.size()) {
            const auto lead = static_cast<unsigned char>(pending_[i]);
            const std::size_t n = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2
                : lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
            if (!n) throw std::runtime_error("Invalid UTF-8 leading byte");
            const auto available = pending_.size() - i;
            for (std::size_t j = 1; j < n && j < available; ++j) {
                const auto b = static_cast<unsigned char>(pending_[i + j]);
                if ((b & 0xc0) != 0x80 || (j == 1 &&
                    ((lead == 0xe0 && b < 0xa0) || (lead == 0xed && b >= 0xa0) ||
                     (lead == 0xf0 && b < 0x90) || (lead == 0xf4 && b >= 0x90))))
                    throw std::runtime_error("Invalid UTF-8 continuation");
            }
            if (available < n) break;
            i += n;
        }
        auto complete = pending_.substr(0, i);
        pending_.erase(0, i);
        return complete;
    }
    bool complete() const noexcept { return pending_.empty(); }
    void reset() noexcept { pending_.clear(); }
private:
    std::string pending_;
};
}
