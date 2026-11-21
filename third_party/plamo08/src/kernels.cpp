// Derived from pfnet/plamo-translate-cli, src/plamo_translate/servers/mlx/
// plamo3_metal.py (Apache-2.0). Adapted for the official PLaMo 3 Translate 0.8B
// checkpoint: BF16 activations, dense BF16 output head, 16-query/1-KV attention,
// source-compatible rounding and stable online softmax.
#include "plamo08/kernels.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include <mlx/fast.h>
#include <mlx/mlx.h>

namespace plamo08 {
namespace {

constexpr int kRows = 8;
constexpr int kThreads = 64;
constexpr int kAttentionGroups = 32;
constexpr int kNormGroups = 16;

using Kernel = mx::fast::CustomKernelFunction;
using TemplateArgs = std::vector<std::pair<std::string, mx::fast::TemplateArg>>;

// A SIMD group computes four rows. Each lane consumes sixteen activations,
// covering a block of 512 columns. All 0.8B projection dimensions fit exactly.
const std::string kMatvecPrelude = R"metal(
uint lane = thread_index_in_simdgroup;
uint row = threadgroup_position_in_grid.x * 8 + simdgroup_index_in_threadgroup * 4;
)metal";

const std::string kLoadQuantized = R"metal(
const device T* xl = x + k + lane * 16;
float sum = 0.0f;
float xt[16];
for (uint i = 0; i < 4; ++i) {
    float x0 = float(xl[4 * i]), x1 = float(xl[4 * i + 1]);
    float x2 = float(xl[4 * i + 2]), x3 = float(xl[4 * i + 3]);
    sum += x0 + x1 + x2 + x3;
    xt[4 * i] = x0;
    xt[4 * i + 1] = x1 / 16.0f;
    xt[4 * i + 2] = x2 / 256.0f;
    xt[4 * i + 3] = x3 / 4096.0f;
}
)metal";

const std::string kQuantizedHeader = R"metal(
inline float qdot4(const device uint* w, thread const float* xt,
                   float sum, float scale, float bias) {
    uint a = w[0], c = w[1];
    float accum = xt[0] * float(a & 15u)
        + xt[1] * float(a & 0xf0u)
        + xt[2] * float(a & 0xf00u)
        + xt[3] * float(a & 0xf000u)
        + xt[4] * float((a >> 16) & 15u)
        + xt[5] * float((a >> 16) & 0xf0u)
        + xt[6] * float((a >> 16) & 0xf00u)
        + xt[7] * float(a >> 28 << 12)
        + xt[8] * float(c & 15u)
        + xt[9] * float(c & 0xf0u)
        + xt[10] * float(c & 0xf00u)
        + xt[11] * float(c & 0xf000u)
        + xt[12] * float((c >> 16) & 15u)
        + xt[13] * float((c >> 16) & 0xf0u)
        + xt[14] * float((c >> 16) & 0xf00u)
        + xt[15] * float(c >> 28 << 12);
    return scale * accum + bias * sum;
}
)metal";

const std::string kDenseHeader = R"metal(
template <typename W>
inline float dot16(const device W* w, thread const float* xt) {
    float accum = 0.0f;
    for (uint i = 0; i < 16; ++i) accum += xt[i] * float(w[i]);
    return accum;
}
)metal";

const std::string kLoadDense = R"metal(
const device T* xl = x + k + lane * 16;
float xt[16];
for (uint i = 0; i < 16; ++i) xt[i] = float(xl[i]);
)metal";

std::string accumulate(const std::string& prefix, const std::string& offset,
                       bool quantized) {
    std::string result;
    for (int i = 0; i != 4; ++i) {
        const std::string row = "(row + " + std::to_string(i) + " + " + offset + ")";
        result += prefix + std::to_string(i) + " += ";
        if (quantized) {
            result += "qdot4(w + " + row + " * (cols / 8) + k / 8 + lane * 2, xt, sum, "
                "float(scales[" + row + " * (cols / 64) + k / 64 + lane / 4]), "
                "float(biases[" + row + " * (cols / 64) + k / 64 + lane / 4]));\n";
        } else {
            result += "dot16(w + " + row + " * cols + k + lane * 16, xt);\n";
        }
    }
    return result;
}

std::string matvec_source(bool quantized) {
    return kMatvecPrelude + R"metal(
float r0 = 0.0f, r1 = 0.0f, r2 = 0.0f, r3 = 0.0f;
for (uint k = 0; k < cols; k += 512) {
)metal" + (quantized ? kLoadQuantized : kLoadDense)
        + accumulate("r", "0", quantized) + R"metal(
}
r0 = simd_sum(r0); r1 = simd_sum(r1);
r2 = simd_sum(r2); r3 = simd_sum(r3);
if (lane == 0) {
    y[row] = T(r0); y[row + 1] = T(r1);
    y[row + 2] = T(r2); y[row + 3] = T(r3);
}
)metal";
}

std::string gate_source() {
    return kMatvecPrelude + R"metal(
float g0 = 0.0f, g1 = 0.0f, g2 = 0.0f, g3 = 0.0f;
float u0 = 0.0f, u1 = 0.0f, u2 = 0.0f, u3 = 0.0f;
for (uint k = 0; k < cols; k += 512) {
)metal" + kLoadQuantized + accumulate("g", "0", true)
        + accumulate("u", "half_rows", true) + R"metal(
}
g0 = simd_sum(g0); g1 = simd_sum(g1);
g2 = simd_sum(g2); g3 = simd_sum(g3);
u0 = simd_sum(u0); u1 = simd_sum(u1);
u2 = simd_sum(u2); u3 = simd_sum(u3);
if (lane < 4) {
    // The official model rounds the projection, SiLU, and product separately.
    float g = float(T(lane == 0 ? g0 : lane == 1 ? g1 : lane == 2 ? g2 : g3));
    float u = float(T(lane == 0 ? u0 : lane == 1 ? u1 : lane == 2 ? u2 : u3));
    float sigmoid = 1.0f / (1.0f + exp(-g));
    T activated = T(g * sigmoid);
    y[row + lane] = T(float(activated) * u);
}
)metal";
}

const std::string kConstants = R"metal(
constant float eps = 1.0e-6f;
constant float attention_scale = 0.08838834764831845f;
)metal";

const std::string kReduce = R"metal(
sum = simd_sum(sum);
if (lane == 0) partial[sg] = sum;
threadgroup_barrier(mem_flags::mem_threadgroup);
s = rsqrt(simd_sum(lane < groups ? partial[lane] : 0.0f) / float(n) + eps);
threadgroup_barrier(mem_flags::mem_threadgroup);
)metal";

const std::string kNormSource = R"metal(
threadgroup float partial[groups];
uint sg = simdgroup_index_in_threadgroup;
uint lane = thread_index_in_simdgroup;
uint at = sg * 32 + lane;
uint steps = n / (groups * 32);
float sum = 0.0f, s;
for (uint i = 0; i < steps; ++i) {
    float v = float(a[at + i * groups * 32]);
    sum += v * v;
}
)metal" + kReduce + R"metal(
sum = 0.0f;
for (uint i = 0; i < steps; ++i) {
    uint j = at + i * groups * 32;
    T normalized = T(float(a[j]) * s);
    T scaled = T(float(normalized) * wa[j]);
    T t = T(float(b[j]) + float(scaled));
    r[j] = t;
    sum += float(t) * float(t);
}
)metal" + kReduce + R"metal(
for (uint i = 0; i < steps; ++i) {
    uint j = at + i * groups * 32;
    normed[j] = T(float(T(float(r[j]) * s)) * wb[j]);
}
)metal";

const std::string kEmbedSource = R"metal(
threadgroup float partial[groups];
uint sg = simdgroup_index_in_threadgroup;
uint lane = thread_index_in_simdgroup;
uint at = sg * 32 + lane;
uint steps = n / (groups * 32);
const device T* e = table + uint(token[0]) * n;
float sum = 0.0f, s;
for (uint i = 0; i < steps; ++i) {
    float v = float(e[at + i * groups * 32]);
    sum += v * v;
}
)metal" + kReduce + R"metal(
for (uint i = 0; i < steps; ++i) {
    uint j = at + i * groups * 32;
    T v = e[j];
    r[j] = v;
    normed[j] = T(float(T(float(v) * s)) * wb[j]);
}
)metal";

const std::string kAttentionSource = R"metal(
// One threadgroup per query head; all sixteen query heads share one KV head.
threadgroup float qh[128], ks[128], partial[groups * 128];
threadgroup float denoms[groups], maxima[groups];
uint head = threadgroup_position_in_grid.x;
uint sg = simdgroup_index_in_threadgroup;
uint lane = thread_index_in_simdgroup;
uint slot = params[1], stored = params[2];
const device T* table = rope + params[0] * 128;
const device T* own_key = qkv + 2048;
const device T* own_value = qkv + 2048 + 128 + lane;
if (sg < 2) {
    const device T* src = sg == 0 ? qkv + head * 128 : own_key;
    const device float* nw = sg == 0 ? q_weight : k_weight;
    float a0 = float(src[lane]), a1 = float(src[lane + 32]);
    float a2 = float(src[lane + 64]), a3 = float(src[lane + 96]);
    float s = rsqrt(simd_sum(a0*a0 + a1*a1 + a2*a2 + a3*a3) / 128.0f + eps);
    a0 = float(T(float(T(a0 * s)) * nw[lane]));
    a1 = float(T(float(T(a1 * s)) * nw[lane + 32]));
    a2 = float(T(float(T(a2 * s)) * nw[lane + 64]));
    a3 = float(T(float(T(a3 * s)) * nw[lane + 96]));
    float c0 = float(table[lane]), c1 = float(table[lane + 32]);
    float s0 = float(table[lane + 64]), s1 = float(table[lane + 96]);
    // SourceRoPE has a BF16 rounding after each multiplication and the sum.
    T r0 = T(float(T(a0*c0)) - float(T(a2*s0)));
    T r1 = T(float(T(a1*c1)) - float(T(a3*s1)));
    T r2 = T(float(T(a2*c0)) + float(T(a0*s0)));
    T r3 = T(float(T(a3*c1)) + float(T(a1*s1)));
    if (sg == 0) {
        qh[lane] = float(r0); qh[lane + 32] = float(r1);
        qh[lane + 64] = float(r2); qh[lane + 96] = float(r3);
    } else {
        ks[lane] = float(r0); ks[lane + 32] = float(r1);
        ks[lane + 64] = float(r2); ks[lane + 96] = float(r3);
        if (head == 0) {
            // Every head reads its current K/V from qkv, never from this slot.
            // The following projection depends on this complete dispatch.
            device T* kw = const_cast<device T*>(keys) + slot * 128 + lane;
            kw[0] = r0; kw[32] = r1; kw[64] = r2; kw[96] = r3;
            device T* vw = const_cast<device T*>(values) + slot * 128 + lane;
            vw[0] = own_value[0]; vw[32] = own_value[32];
            vw[64] = own_value[64]; vw[96] = own_value[96];
        }
    }
}
threadgroup_barrier(mem_flags::mem_threadgroup);
float q0 = qh[lane], q1 = qh[lane + 32];
float q2 = qh[lane + 64], q3 = qh[lane + 96];
float own_score = simd_sum(q0*ks[lane] + q1*ks[lane + 32]
    + q2*ks[lane + 64] + q3*ks[lane + 96]) * attention_scale;
// Independent online softmax reductions, with the current token in group 0.
float m = sg == 0 ? own_score : -INFINITY;
float den = sg == 0 ? 1.0f : 0.0f;
float a0 = sg == 0 ? float(own_value[0]) : 0.0f;
float a1 = sg == 0 ? float(own_value[32]) : 0.0f;
float a2 = sg == 0 ? float(own_value[64]) : 0.0f;
float a3 = sg == 0 ? float(own_value[96]) : 0.0f;
for (uint j = sg; j < stored; j += groups) {
    if (j == slot) continue;
    const device T* kr = keys + j * 128 + lane;
    const device T* vr = values + j * 128 + lane;
    float score = simd_sum(q0*float(kr[0]) + q1*float(kr[32])
        + q2*float(kr[64]) + q3*float(kr[96])) * attention_scale;
    float next_m = max(m, score);
    float alpha = den > 0.0f ? exp(m - next_m) : 0.0f;
    float beta = exp(score - next_m);
    den = den * alpha + beta;
    a0 = a0 * alpha + float(vr[0]) * beta;
    a1 = a1 * alpha + float(vr[32]) * beta;
    a2 = a2 * alpha + float(vr[64]) * beta;
    a3 = a3 * alpha + float(vr[96]) * beta;
    m = next_m;
}
uint at = sg * 128 + lane;
partial[at] = a0; partial[at + 32] = a1;
partial[at + 64] = a2; partial[at + 96] = a3;
if (lane == 0) { denoms[sg] = den; maxima[sg] = m; }
threadgroup_barrier(mem_flags::mem_threadgroup);
if (sg < 4) {
    float peak = simd_max(lane < groups ? maxima[lane] : -INFINITY);
    float factor = lane < groups && denoms[lane] > 0.0f
        ? exp(maxima[lane] - peak) : 0.0f;
    float total = simd_sum(lane < groups ? denoms[lane] * factor : 0.0f);
    float acc = 0.0f;
    for (uint i = 0; i < groups; ++i) {
        if (denoms[i] > 0.0f)
            acc += partial[i * 128 + sg * 32 + lane] * exp(maxima[i] - peak);
    }
    out[head * 128 + sg * 32 + lane] = T(acc / total);
}
)metal";

Kernel make_kernel(const std::string& name, const std::vector<std::string>& inputs,
                   const std::vector<std::string>& outputs, const std::string& source,
                   const std::string& header) {
    mx::CompileOptions options;
    options.math_mode = mx::MathMode::Safe;
    return mx::fast::metal_kernel(name, inputs, outputs, source, header, true, false, options);
}

}  // namespace

struct FastDecoder::Impl {
    struct Norms {
        mx::array post_mixer, pre_mlp, post_mlp, following, q, k;
    };

    const Weights& weights;
    const RopeTables& rope;
    mx::Stream stream;
    std::vector<Norms> norms;
    mx::array first_norm;
    Kernel qmv, qgate, dmv, norm, embed, attention;

    mx::array scale(const mx::array& weight, float offset) const {
        return mx::astype(mx::add(weight, mx::array(offset, weight.dtype()), stream),
                          mx::float32, stream);
    }

    Impl(const Weights& weights, const RopeTables& rope)
        : weights(weights), rope(rope), stream(mx::default_stream(mx::Device::gpu)),
          first_norm(scale(weights.layers.front().pre_mixer, 1.0f)),
          qmv(make_kernel("plamo08_qmv", {"w", "scales", "biases", "x"}, {"y"},
                          matvec_source(true), kQuantizedHeader)),
          qgate(make_kernel("plamo08_qgate", {"w", "scales", "biases", "x"}, {"y"},
                            gate_source(), kQuantizedHeader)),
          dmv(make_kernel("plamo08_dmv", {"w", "x"}, {"y"}, matvec_source(false), kDenseHeader)),
          norm(make_kernel("plamo08_norm", {"a", "b", "wa", "wb"}, {"r", "normed"},
                           kNormSource, kConstants)),
          embed(make_kernel("plamo08_embed", {"table", "token", "wb"}, {"r", "normed"},
                            kEmbedSource, kConstants)),
          attention(make_kernel("plamo08_attention", {"qkv", "keys", "values", "rope", "params",
                                "q_weight", "k_weight"}, {"out"}, kAttentionSource, kConstants)) {
        std::vector<mx::array> ready{first_norm};
        norms.reserve(spec::layers);
        for (int i = 0; i != spec::layers; ++i) {
            const auto& layer = weights.layers[i];
            const auto& following = i + 1 < spec::layers
                ? weights.layers[i + 1].pre_mixer : weights.final_norm;
            norms.push_back({scale(layer.post_mixer, spec::post_mixer_offset),
                             scale(layer.pre_mlp, 1.0f),
                             scale(layer.post_mlp, spec::post_mlp_offset),
                             scale(following, 1.0f),
                             scale(layer.q_norm, 1.0f), scale(layer.k_norm, 1.0f)});
            const auto& n = norms.back();
            ready.insert(ready.end(), {n.post_mixer, n.pre_mlp, n.post_mlp, n.following, n.q, n.k});
        }
        mx::eval(ready);
    }

    mx::array mv(const Linear& linear, const mx::array& x, bool gate = false) {
        const int rows = gate ? linear.out_features / 2 : linear.out_features;
        const int groups = rows / kRows;
        TemplateArgs args{{"T", mx::bfloat16}, {"cols", linear.in_features}};
        if (gate) args.emplace_back("half_rows", rows);
        std::vector<mx::array> inputs{linear.weight};
        if (linear.quantized()) {
            inputs.push_back(*linear.scales);
            inputs.push_back(*linear.biases);
        }
        inputs.push_back(x);
        auto& kernel = linear.quantized() ? (gate ? qgate : qmv) : dmv;
        return kernel(inputs, {{rows}}, {mx::bfloat16},
                      {groups * kThreads, 1, 1}, {kThreads, 1, 1}, args,
                      std::nullopt, false, stream).front();
    }

    std::vector<mx::array> pair(const mx::array& a, const mx::array& b,
                              const mx::array& wa, const mx::array& wb) {
        return norm({a, b, wa, wb}, {{spec::hidden}, {spec::hidden}},
                    {mx::bfloat16, mx::bfloat16}, {kNormGroups * 32, 1, 1},
                    {kNormGroups * 32, 1, 1},
                    {{"T", mx::bfloat16}, {"n", spec::hidden}, {"groups", kNormGroups}},
                    std::nullopt, false, stream);
    }

    mx::array logits(const mx::array& token, std::vector<KVCache>& caches) {
        const auto position = static_cast<std::uint32_t>(caches.front().offset);
        const auto window = static_cast<std::uint32_t>(spec::window);
        // The two attention families share positions; keep host allocation out
        // of the layer loop, including allocations of tiny parameter arrays.
        mx::array local_params({position, position % window, std::min(position, window)}, mx::uint32);
        mx::array full_params({position, position, position}, mx::uint32);
        auto initial = embed({weights.embedding, token, first_norm},
                             {{spec::hidden}, {spec::hidden}}, {mx::bfloat16, mx::bfloat16},
                             {kNormGroups * 32, 1, 1}, {kNormGroups * 32, 1, 1},
                             {{"T", mx::bfloat16}, {"n", spec::hidden}, {"groups", kNormGroups}},
                             std::nullopt, false, stream);
        mx::array x = initial[0], normed = initial[1];
        for (int i = 0; i != spec::layers; ++i) {
            const auto& layer = weights.layers[i];
            const auto& n = norms[i];
            auto& cache = caches[i];
            auto qkv = mv(layer.qkv, normed);
            auto attended = attention({qkv, cache.keys, cache.values,
                                       cache.full ? rope.full : rope.local,
                                       cache.full ? full_params : local_params, n.q, n.k},
                                      {{spec::q_dim}}, {mx::bfloat16},
                                      {kAttentionGroups * 32 * spec::q_heads, 1, 1},
                                      {kAttentionGroups * 32, 1, 1},
                                      {{"T", mx::bfloat16}, {"groups", kAttentionGroups}},
                                      std::nullopt, false, stream).front();
            ++cache.offset;
            auto projected = mv(layer.out, attended);
            auto first = pair(projected, x, n.post_mixer, n.pre_mlp);
            x = first[0];
            normed = first[1];
            auto hidden = mv(layer.gate_up, normed, true);
            projected = mv(layer.down, hidden);
            auto second = pair(projected, x, n.post_mlp, n.following);
            x = second[0];
            normed = second[1];
        }
        // The official dense BF16 output projection rounds before converting
        // its public logits to FP32; retain that boundary for greedy decisions.
        return mx::astype(mv(weights.head, normed), mx::float32, stream);
    }
};

FastDecoder::FastDecoder(const Weights& weights, const RopeTables& rope)
    : impl_(std::make_unique<Impl>(weights, rope)) {}
FastDecoder::~FastDecoder() = default;
FastDecoder::FastDecoder(FastDecoder&&) noexcept = default;
FastDecoder& FastDecoder::operator=(FastDecoder&&) noexcept = default;

mx::array FastDecoder::logits(mx::array token, std::vector<KVCache>& caches) {
    return impl_->logits(token, caches);
}

mx::array FastDecoder::next_token(mx::array token, std::vector<KVCache>& caches) {
    return mx::astype(mx::argmax(impl_->logits(token, caches), true, impl_->stream),
                      mx::uint32, impl_->stream);
}

}  // namespace plamo08
