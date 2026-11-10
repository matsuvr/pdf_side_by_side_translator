#include "ggml-cpu/quants.h"
#include "ggml-cpu/ops.h"
#include "ggml-cpu/vec.h"
#include <intrin.h>
#include <cstdlib>
#include <cstring>

namespace {
bool can_use_avx2() {
    // A one-way override also lets the integration probe exercise the
    // portable path on a modern development machine.
    const char* mode = std::getenv("OPT_CPU_KERNELS");
    if (mode && std::strcmp(mode, "portable") == 0) return false;

    int regs[4];
    __cpuid(regs, 0);
    if (regs[0] < 7) return false;
    __cpuidex(regs, 1, 0);
    // SSE3, SSSE3, FMA, SSE4.1/4.2, POPCNT, OSXSAVE, AVX and F16C.
    constexpr unsigned required = (1u << 0)  | (1u << 9)  | (1u << 12) | (1u << 19) | (1u << 20) |
                                  (1u << 23) | (1u << 27) | (1u << 28) | (1u << 29);
    if ((static_cast<unsigned>(regs[2]) & required) != required) return false;
    if ((_xgetbv(0) & 6) != 6) return false; // OS saves both XMM and YMM.
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0;
}
const bool use_avx2 = can_use_avx2();
}

extern "C" int opt_windows_has_avx2_kernels() {
    return use_avx2;
}

// Both implementations have ggml's exact C ABI. The feature test runs once,
// outside the inner products and before any ISA-specific call.
#define OPT_DISPATCH(name, params, args)                   \
    extern "C" decltype(name) name##_portable, name##_avx2; \
    extern "C" void name params {                           \
        (use_avx2 ? name##_avx2 : name##_portable) args;    \
    }
#define OPT_ROW(name) OPT_DISPATCH(name, (const float* x, void* y, int64_t k), (x, y, k))
#define OPT_DOT(name) OPT_DISPATCH(name, \
    (int n, float* s, size_t bs, const void* x, size_t bx, const void* y, size_t by, int nrc), \
    (n, s, bs, x, bx, y, by, nrc))
#include "quant_kernels.inc"

OPT_DISPATCH(ggml_compute_forward_flash_attn_ext,
    (const ggml_compute_params* params, ggml_tensor* dst), (params, dst))
OPT_DISPATCH(ggml_vec_dot_f16_f32,
    (int n, float* s, size_t bs, const ggml_fp16_t* x, size_t bx, const float* y, size_t by, int nrc),
    (n, s, bs, x, bx, y, by, nrc))
OPT_DISPATCH(ggml_vec_swiglu_f32, (int n, float* y, const float* x, const float* gate), (n, y, x, gate))
