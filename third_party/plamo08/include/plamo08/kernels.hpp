#pragma once

#include <memory>

#include "plamo08/weights.hpp"

namespace plamo08 {

// Fused single-token decoder for the official 0.8B MLX affine-4bit checkpoint.
// Weights and RoPE tables must outlive this object. Calls share the caller's
// caches and must be serialized with prefill, cache resets and other decoders.
class FastDecoder {
public:
    FastDecoder(const Weights& weights, const RopeTables& rope);
    ~FastDecoder();
    FastDecoder(FastDecoder&&) noexcept;
    FastDecoder& operator=(FastDecoder&&) noexcept;
    FastDecoder(const FastDecoder&) = delete;
    FastDecoder& operator=(const FastDecoder&) = delete;

    mx::array logits(mx::array token, std::vector<KVCache>& caches);
    mx::array next_token(mx::array token, std::vector<KVCache>& caches);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace plamo08
