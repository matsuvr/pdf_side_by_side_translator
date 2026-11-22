#include "inference/platform_backend.hpp"

namespace opt {
ggml_backend_dev_t preferred_gpu() {
    const auto metal = ggml_backend_reg_by_name("MTL");
    if (!metal) return nullptr;
    for (std::size_t i = 0; i < ggml_backend_reg_dev_count(metal); ++i) {
        const auto device = ggml_backend_reg_dev_get(metal, i);
        const auto type = ggml_backend_dev_type(device);
        if (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU)
            return device;
    }
    return nullptr;
}

const char* backend_name(bool cpu) { return cpu ? "CPU" : "Metal"; }
int greedy_token(const float* logits, int count) { return scalar_greedy_token(logits, count); }
}
