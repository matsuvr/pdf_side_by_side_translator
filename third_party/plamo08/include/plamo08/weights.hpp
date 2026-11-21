#pragma once

#include <optional>
#include <vector>
#include <mlx/array.h>

#include "plamo08/config.hpp"

namespace plamo08 {
namespace mx = mlx::core;

struct Linear {
    mx::array weight;
    std::optional<mx::array> scales;
    std::optional<mx::array> biases;
    int in_features;
    int out_features;
    bool quantized() const { return scales.has_value(); }
};

struct LayerWeights {
    Linear qkv, out, gate_up, down;
    mx::array pre_mixer, post_mixer, pre_mlp, post_mlp, q_norm, k_norm;
};

struct Weights {
    mx::array embedding;
    Linear head;
    mx::array final_norm;
    std::vector<LayerWeights> layers;
};

struct KVCache {
    mx::array keys, values;  // [1, capacity, 128], BF16, rotated keys
    int offset;
    int capacity;
    bool full;
};

struct RopeTables {
    mx::array local, full;  // [context,128], cosine half then sine half
};
}  // namespace plamo08
