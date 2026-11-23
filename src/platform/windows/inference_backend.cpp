#include "inference/platform_backend.hpp"

extern "C" int opt_windows_has_avx2_kernels();
extern "C" int opt_windows_has_vnni_kernels();
int opt_greedy_avx2(const float* logits, int count);

namespace opt {
ggml_backend_dev_t preferred_gpu() { return nullptr; }

const char* backend_name(bool) {
    if (opt_windows_has_vnni_kernels()) return "CPU (AVX-VNNI)";
    return opt_windows_has_avx2_kernels() ? "CPU (AVX2)" : "CPU (portable)";
}

int greedy_token(const float* logits, int count) {
    if (opt_windows_has_avx2_kernels()) return opt_greedy_avx2(logits, count);
    return scalar_greedy_token(logits, count);
}
}
