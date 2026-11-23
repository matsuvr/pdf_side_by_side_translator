#pragma once

#include "core/utf8.hpp"
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opt {
// Index decoded vocabulary pieces once. PLaMo can encode a single Unicode
// scalar as several tokens, which must remain eligible in the right order.
class Utf8TokenFilter {
public:
    void add(std::int32_t id, std::string_view piece) {
        if (unsupported_text_control(piece) != std::string_view::npos) {
            invalid_starters_.push_back(id);
            return;
        }
        if (complete_utf8_prefix(piece) != std::string_view::npos) return;
        invalid_starters_.push_back(id);
        if ((static_cast<unsigned char>(piece.front()) & 0xc0) == 0x80)
            continuations_.push_back({id, std::string(piece)});
    }

    void mask_invalid_starters(float* logits) const {
        for (const auto id : invalid_starters_)
            logits[id] = -std::numeric_limits<float>::infinity();
    }

    // Called only inside a scalar. Stops and ordinary word tokens cannot
    // interrupt it. Entries are in vocabulary order for greedy tie breaking.
    std::int32_t best_continuation(const float* logits, const Utf8Stream& decoder) const {
        std::int32_t best = -1;
        float score = -std::numeric_limits<float>::infinity();
        for (const auto& [id, piece] : continuations_) {
            if (logits[id] > score && decoder.accepts(piece)) {
                best = id;
                score = logits[id];
            }
        }
        return best;
    }

private:
    std::vector<std::int32_t> invalid_starters_;
    std::vector<std::pair<std::int32_t, std::string>> continuations_;
};
}
