#pragma once

#include "ggml-backend.h"
#include <algorithm>

namespace opt {
// Implementations are selected at build time. A null device requests CPU inference.
ggml_backend_dev_t preferred_gpu();
const char* backend_name(bool cpu);
int greedy_token(const float* logits, int count);

inline int scalar_greedy_token(const float* logits, int count) {
    return static_cast<int>(std::max_element(logits, logits + count) - logits);
}
}
