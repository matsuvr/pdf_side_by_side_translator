#pragma once

namespace plamo08 {
namespace spec {
inline constexpr int hidden = 1536;
inline constexpr int layers = 16;
inline constexpr int q_heads = 16;
inline constexpr int kv_heads = 1;
inline constexpr int head_dim = 128;
inline constexpr int q_dim = q_heads * head_dim;
inline constexpr int kv_dim = kv_heads * head_dim;
inline constexpr int intermediate = 6144;
inline constexpr int vocab = 107520;
inline constexpr int window = 2048;
inline constexpr int group_size = 64;
inline constexpr int bits = 4;
inline constexpr float eps = 1.0e-6f;
inline constexpr float post_mixer_offset = 0.2f;
inline constexpr float post_mlp_offset = 0.08944271909999159f;
inline constexpr bool full_attention(int layer) { return layer == 7 || layer == 15; }
}  // namespace spec
}  // namespace plamo08
