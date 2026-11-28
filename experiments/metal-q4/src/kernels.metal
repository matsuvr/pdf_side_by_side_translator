#include <metal_stdlib>
using namespace metal;

// ============================================================================
// Apple Silicon (Apple7+ / M1..M4) Specialized Metal 3.1 Kernels
// for PLaMo 3 Translate 0.8B (D=1536, 16Q:1KV MQA, H=128, FFN=6144, V=107520)
//
// Losslessly repacked 16-byte-aligned superblocks matching GGUF Q4_K / Q6_K:
//   Q4K_M  (512 weights = 288 bytes, 4.50 bpw):
//     [hdr0 (16 B) | hdr1 (16 B) | linear q4 (256 B)]
//   Q6K_M  (512 weights = 432 bytes, 6.75 bpw):
//     [d0,d1 (4 B) | pad (12 B) | scales[32] (32 B) | linear ql (256 B) | linear qh (128 B)]
//   LM-Head Screen1 (1536 weights = 240 bytes/row, 1.25 bpw):
//     [24 x half scales (48 B) | 48 x uint signs (192 B)]
//
// Residual stream, RMSNorm, RoPE, matvec/GEMM accumulators, softmax, and logits are FP32;
// intermediate activation buffers (norm, QKV/FFN scratch) and KV cache are FP16.
// ============================================================================

constant float INV_D = 1.0f / 1536.0f;
constant float INV_H = 1.0f / 128.0f;
constant float ATTN_SCALE_LOG2E = 0.08838834764831845f * 1.4426950408889634f;

// Zero-shift 4-bit dot product: pre-scaled x by (1, 1/16, 1/256, 1/4096)
#define QDOT_U16(u, fx, d) \
    d = fma(float((u) & 0x000Fu), (fx).x, d); \
    d = fma(float((u) & 0x00F0u), (fx).y, d); \
    d = fma(float((u) & 0x0F00u), (fx).z, d); \
    d = fma(float((u) & 0xF000u), (fx).w, d);

static inline float qdot16_u2(uint2 qw, float4 x0, float4 x1, float4 x2, float4 x3) {
    ushort4 u = as_type<ushort4>(qw);
    float d = 0.0f;
    QDOT_U16(u.x, x0, d);
    QDOT_U16(u.y, x1, d);
    QDOT_U16(u.z, x2, d);
    QDOT_U16(u.w, x3, d);
    return d;
}

// Zero-shift 2-bit high-part dot product: pre-scaled h by (16, 4, 1, 0.25)
// Computes 16 * sum_{i=0..15} hi2[i] * x[i] with zero right-shifts.
#define QDOT_HI2_B(b, fh, d) \
    d = fma(float((b) & 0x03u), (fh).x, d); \
    d = fma(float((b) & 0x0Cu), (fh).y, d); \
    d = fma(float((b) & 0x30u), (fh).z, d); \
    d = fma(float((b) & 0xC0u), (fh).w, d);

static inline float qdot16_hi2(uint qh, float4 h0, float4 h1, float4 h2, float4 h3) {
    uchar4 b = as_type<uchar4>(qh);
    float d = 0.0f;
    QDOT_HI2_B(b.x, h0, d);
    QDOT_HI2_B(b.y, h1, d);
    QDOT_HI2_B(b.z, h2, d);
    QDOT_HI2_B(b.w, h3, d);
    return d;
}

// Exact GGML Q4_K 6-bit scale/min decode for sub-block g in 0..7 (single 128-bit load, branchless)
static inline void decode_q4k_sm(device const uchar* hdr16, uint g, thread float& sc, thread float& mn) {
    uint4 h = *(device const uint4*)hdr16;
    half2 dm = as_type<half2>(h.x);
    uint shift = (g & 3u) * 8u;
    uint sy = (h.y >> shift) & 0xFFu;
    uint sz = (h.z >> shift) & 0xFFu;
    uint sw = (h.w >> shift) & 0xFFu;
    bool lo = (g < 4u);
    uint sc6 = lo ? (sy & 63u) : ((sw & 15u) | ((sy >> 6) << 4));
    uint m6 = lo ? (sz & 63u) : ((sw >> 4) | ((sz >> 6) << 4));
    sc = float(dm.x) * float(sc6);
    mn = -float(dm.y) * float(m6);
}

// ----------------------------------------------------------------------------
// 1. Fused Embed (Q4K_M superblock) + Layer-0 Pre-Mixer RMSNorm
//    Grid: (n, 1, 1), Threads: 192 (6 SIMD groups, 8 dims/thread)
// ----------------------------------------------------------------------------
struct EmbedParams {
    uint ids_offset;
    uint use_state;
};

[[max_total_threads_per_threadgroup(192)]]
kernel void embed_and_norm_q4k(
    device const uchar* w_sb [[buffer(0)]],
    device const uint* ids [[buffer(1)]],
    device const float4* norm_w [[buffer(2)]],
    device float4* x_out [[buffer(3)]],
    device half4* norm_out [[buffer(4)]],
    device const uint* state [[buffer(5)]],
    constant EmbedParams& params [[buffer(6)]],
    uint tg_id [[threadgroup_position_in_grid]],
    uint tid [[thread_index_in_threadgroup]],
    uint sg_id [[simdgroup_index_in_threadgroup]],
    uint lane [[thread_index_in_simdgroup]]
) {
    if (params.use_state != 0u && state[0] != 0u) {
        return;
    }
    uint token_id = (params.use_state != 0u) ? state[2u + params.ids_offset] : ids[params.ids_offset + tg_id];

    uint sb = tid >> 6;
    uint lw = tid & 63u;
    uint blk = lw >> 5;       // 0 or 1 (256-weight Q4_K block)
    uint g = (lw >> 2) & 7u;  // 0..7 (32-weight sub-block)
    device const uchar* sb_ptr = w_sb + token_id * 864u + sb * 288u;
    float sc, mn;
    decode_q4k_sm(sb_ptr + blk * 16u, g, sc, mn);
    uint bits = ((device const uint*)(sb_ptr + 32u))[lw];

    float4 v0 = fma(float4(
        float(bits & 15u),
        float((bits >> 4) & 15u),
        float((bits >> 8) & 15u),
        float((bits >> 12) & 15u)
    ), sc, mn);
    float4 v1 = fma(float4(
        float((bits >> 16) & 15u),
        float((bits >> 20) & 15u),
        float((bits >> 24) & 15u),
        float(bits >> 28)
    ), sc, mn);

    uint out_idx = tg_id * 384u + tid * 2u;
    x_out[out_idx] = v0;
    x_out[out_idx + 1u] = v1;

    threadgroup float sg_sq[6];
    float sq = simd_sum(dot(v0, v0) + dot(v1, v1));
    if (lane == 0u) {
        sg_sq[sg_id] = sq;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    float total_sq = sg_sq[0] + sg_sq[1] + sg_sq[2] + sg_sq[3] + sg_sq[4] + sg_sq[5];
    float inv = rsqrt(total_sq * INV_D + 1e-6f);

    norm_out[out_idx] = half4(v0 * inv * norm_w[tid * 2u]);
    norm_out[out_idx + 1u] = half4(v1 * inv * norm_w[tid * 2u + 1u]);
}

// ----------------------------------------------------------------------------
// 2. Fused Post-Norm + Residual Add + Next Pre-Norm (or Final Norm)
//    Grid: (n, 1, 1), Threads: 192
// ----------------------------------------------------------------------------
struct NormParams {
    uint token_offset;
    uint write_res;
    uint check_stop;
};

[[max_total_threads_per_threadgroup(192)]]
kernel void fused_post_add_pre_norm(
    device const half4* proj [[buffer(0)]],
    device const float4* residual [[buffer(1)]],
    device const float4* post_w [[buffer(2)]],
    device const float4* pre_w [[buffer(3)]],
    device float4* res_out [[buffer(4)]],
    device half4* norm_out [[buffer(5)]],
    device const uint* state [[buffer(6)]],
    constant NormParams& params [[buffer(7)]],
    uint tg_id [[threadgroup_position_in_grid]],
    uint tid [[thread_index_in_threadgroup]],
    uint sg_id [[simdgroup_index_in_threadgroup]],
    uint lane [[thread_index_in_simdgroup]]
) {
    if (params.check_stop != 0u && state[0] != 0u) {
        return;
    }
    uint src_idx = (tg_id + params.token_offset) * 384u + tid * 2u;
    float4 p0 = float4(proj[src_idx]);
    float4 p1 = float4(proj[src_idx + 1u]);

    threadgroup float sg_sq1[6];
    threadgroup float sg_sq2[6];
    float sq1 = simd_sum(dot(p0, p0) + dot(p1, p1));
    if (lane == 0u) {
        sg_sq1[sg_id] = sq1;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    float total_sq1 = sg_sq1[0] + sg_sq1[1] + sg_sq1[2] + sg_sq1[3] + sg_sq1[4] + sg_sq1[5];
    float inv1 = rsqrt(total_sq1 * INV_D + 1e-6f);

    float4 y0 = fma(p0 * inv1, post_w[tid * 2u], residual[src_idx]);
    float4 y1 = fma(p1 * inv1, post_w[tid * 2u + 1u], residual[src_idx + 1u]);

    if (params.write_res != 0u) {
        res_out[src_idx] = y0;
        res_out[src_idx + 1u] = y1;
    }

    float sq2 = simd_sum(dot(y0, y0) + dot(y1, y1));
    if (lane == 0u) {
        sg_sq2[sg_id] = sq2;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    float total_sq2 = sg_sq2[0] + sg_sq2[1] + sg_sq2[2] + sg_sq2[3] + sg_sq2[4] + sg_sq2[5];
    float inv2 = rsqrt(total_sq2 * INV_D + 1e-6f);

    uint dst_idx = tg_id * 384u + tid * 2u;
    norm_out[dst_idx] = half4(y0 * inv2 * pre_w[tid * 2u]);
    norm_out[dst_idx + 1u] = half4(y1 * inv2 * pre_w[tid * 2u + 1u]);
}

// ----------------------------------------------------------------------------
// 3. Prepare QKV: FP32 Q-Norm, K-Norm, RoPE, Write FP16 Q + FP16 KV Cache
//    Grid: (17, n, 1), Threads: 32
// ----------------------------------------------------------------------------
struct PosParams {
    uint start_pos;
    uint check_stop;
};

[[max_total_threads_per_threadgroup(32)]]
kernel void prepare_qkv(
    device const half* qkv [[buffer(0)]],
    device const float* qnorm [[buffer(1)]],
    device const float* knorm [[buffer(2)]],
    device const float* rope [[buffer(3)]],
    device half* q_out [[buffer(4)]],
    device half* keys [[buffer(5)]],
    device half* values [[buffer(6)]],
    device const uint* state [[buffer(7)]],
    constant PosParams& params [[buffer(8)]],
    uint2 tg_pos [[threadgroup_position_in_grid]],
    uint lane [[thread_index_in_simdgroup]]
) {
    if (params.check_stop != 0u && state[0] != 0u) {
        return;
    }
    uint head = tg_pos.x;
    uint token = tg_pos.y;
    uint pos = params.start_pos + token;
    uint base = token * 2304u + head * 128u;

    float a0 = float(qkv[base + lane]);
    float a1 = float(qkv[base + lane + 32u]);
    float b0 = float(qkv[base + lane + 64u]);
    float b1 = float(qkv[base + lane + 96u]);

    float sq = simd_sum(a0 * a0 + a1 * a1 + b0 * b0 + b1 * b1);
    float inv = rsqrt(sq * INV_H + 1e-6f);
    float scale_rope = rope[64];

    float fpos = float(pos);
    float s0, c0, s1, c1;
    s0 = sincos(fpos * rope[lane], c0);
    s1 = sincos(fpos * rope[lane + 32u], c1);
    c0 *= scale_rope; s0 *= scale_rope;
    c1 *= scale_rope; s1 *= scale_rope;

    if (head == 16u) {
        a0 *= inv * knorm[lane];
        a1 *= inv * knorm[lane + 32u];
        b0 *= inv * knorm[lane + 64u];
        b1 *= inv * knorm[lane + 96u];
        uint kv_base = pos * 128u;
        keys[kv_base + lane] = half(a0 * c0 - b0 * s0);
        keys[kv_base + lane + 32u] = half(a1 * c1 - b1 * s1);
        keys[kv_base + lane + 64u] = half(b0 * c0 + a0 * s0);
        keys[kv_base + lane + 96u] = half(b1 * c1 + a1 * s1);
        values[kv_base + lane] = qkv[base + 128u + lane];
        values[kv_base + lane + 32u] = qkv[base + 160u + lane];
        values[kv_base + lane + 64u] = qkv[base + 192u + lane];
        values[kv_base + lane + 96u] = qkv[base + 224u + lane];
    } else {
        a0 *= inv * qnorm[lane];
        a1 *= inv * qnorm[lane + 32u];
        b0 *= inv * qnorm[lane + 64u];
        b1 *= inv * qnorm[lane + 96u];
        uint q_base = (token * 16u + head) * 128u;
        q_out[q_base + lane] = half(a0 * c0 - b0 * s0);
        q_out[q_base + lane + 32u] = half(a1 * c1 - b1 * s1);
        q_out[q_base + lane + 64u] = half(b0 * c0 + a0 * s0);
        q_out[q_base + lane + 96u] = half(b1 * c1 + a1 * s1);
    }
}

// ----------------------------------------------------------------------------
// 4A. Decode Matvec Q4K_M (32-lane uint2, exact GGUF Q4_K, FP16 I/O, FP32 acc)
// ----------------------------------------------------------------------------
struct MatvecParams {
    uint K;
    uint N;
    uint check_stop;
    uint fused_swiglu;
};

[[max_total_threads_per_threadgroup(64)]]
kernel void matvec_q4k_u2_nr2(
    device const uchar* w_sb [[buffer(0)]],
    device const half4* x [[buffer(1)]],
    device half* y [[buffer(2)]],
    device const uint* state [[buffer(3)]],
    constant MatvecParams& p [[buffer(4)]],
    uint2 tg_pos [[threadgroup_position_in_grid]],
    uint sg_id [[simdgroup_index_in_threadgroup]],
    uint lane [[thread_index_in_simdgroup]]
) {
    if (p.check_stop != 0u && state[0] != 0u) {
        return;
    }
    const uint row0 = (tg_pos.x * 2u + sg_id) * 2u;
    const uint token = tg_pos.y;
    const uint K = p.K;
    const uint sbs = K >> 9;
    const uint row_bytes = sbs * 288u;
    const float4 ps = float4(1.0f, 1.0f / 16.0f, 1.0f / 256.0f, 1.0f / 4096.0f);
    const uint blk_off = (lane >> 4) * 16u;
    const uint g = (lane >> 1) & 7u;

    device const half4* x_tok = x + token * (K >> 2);
    device const uchar* rptr0 = w_sb + (row0 + 0u) * row_bytes;
    device const uchar* rptr1 = w_sb + (row0 + 1u) * row_bytes;

    float acc0 = 0.0f, acc1 = 0.0f;

    for (uint sb = 0u; sb < sbs; ++sb) {
        uint ix = (sb * 32u + lane) * 4u;
        float4 r0 = float4(x_tok[ix + 0u]);
        float4 r1 = float4(x_tok[ix + 1u]);
        float4 r2 = float4(x_tok[ix + 2u]);
        float4 r3 = float4(x_tok[ix + 3u]);

        float4 sv = (r0 + r1) + (r2 + r3);
        float sum_x = (sv.x + sv.y) + (sv.z + sv.w);

        float4 x0 = r0 * ps;
        float4 x1 = r1 * ps;
        float4 x2 = r2 * ps;
        float4 x3 = r3 * ps;

        uint off = sb * 288u;
        float sc0, mn0, sc1, mn1;
        decode_q4k_sm(rptr0 + off + blk_off, g, sc0, mn0);
        decode_q4k_sm(rptr1 + off + blk_off, g, sc1, mn1);
        acc0 = fma(qdot16_u2(((device const uint2*)(rptr0 + off + 32u))[lane], x0, x1, x2, x3), sc0, fma(sum_x, mn0, acc0));
        acc1 = fma(qdot16_u2(((device const uint2*)(rptr1 + off + 32u))[lane], x0, x1, x2, x3), sc1, fma(sum_x, mn1, acc1));
    }

    acc0 = simd_sum(acc0);
    acc1 = simd_sum(acc1);

    if (lane == 0u) {
        if (p.fused_swiglu != 0u) {
            y[token * (p.N >> 1) + (row0 >> 1)] = half((acc0 / (1.0f + exp(-acc0))) * acc1);
        } else {
            device half* dst = y + token * p.N + row0;
            dst[0] = half(acc0);
            dst[1] = half(acc1);
        }
    }
}

// ----------------------------------------------------------------------------
// 4B. Decode Matvec Q6K_M (32-lane uint2+uint, exact GGUF Q6_K, FP16 I/O, FP32 acc)
// ----------------------------------------------------------------------------
[[max_total_threads_per_threadgroup(64)]]
kernel void matvec_q6k_u2_nr2(
    device const uchar* w_sb [[buffer(0)]],
    device const half4* x [[buffer(1)]],
    device half* y [[buffer(2)]],
    device const uint* state [[buffer(3)]],
    constant MatvecParams& p [[buffer(4)]],
    uint2 tg_pos [[threadgroup_position_in_grid]],
    uint sg_id [[simdgroup_index_in_threadgroup]],
    uint lane [[thread_index_in_simdgroup]]
) {
    if (p.check_stop != 0u && state[0] != 0u) {
        return;
    }
    const uint row0 = (tg_pos.x * 2u + sg_id) * 2u;
    const uint token = tg_pos.y;
    const uint K = p.K;
    const uint sbs = K >> 9;
    const uint row_bytes = sbs * 432u;
    const float4 ps4 = float4(1.0f, 1.0f / 16.0f, 1.0f / 256.0f, 1.0f / 4096.0f);
    const float4 ps4_to_ps2 = float4(16.0f, 64.0f, 256.0f, 1024.0f);
    const uint blk = lane >> 4;

    device const half4* x_tok = x + token * (K >> 2);
    device const uchar* rptr0 = w_sb + (row0 + 0u) * row_bytes;
    device const uchar* rptr1 = w_sb + (row0 + 1u) * row_bytes;

    float acc0 = 0.0f, acc1 = 0.0f;

    for (uint sb = 0u; sb < sbs; ++sb) {
        uint ix = (sb * 32u + lane) * 4u;
        float4 x0 = float4(x_tok[ix + 0u]);
        float4 x1 = float4(x_tok[ix + 1u]);
        float4 x2 = float4(x_tok[ix + 2u]);
        float4 x3 = float4(x_tok[ix + 3u]);

        float4 sv = (x0 + x1) + (x2 + x3);
        float neg32 = -32.0f * ((sv.x + sv.y) + (sv.z + sv.w));

        x0 *= ps4; x1 *= ps4; x2 *= ps4; x3 *= ps4;

        uint off = sb * 432u;
        float d0 = qdot16_u2(((device const uint2*)(rptr0 + off + 48u))[lane], x0, x1, x2, x3) + neg32;
        float d1 = qdot16_u2(((device const uint2*)(rptr1 + off + 48u))[lane], x0, x1, x2, x3) + neg32;

        x0 *= ps4_to_ps2; x1 *= ps4_to_ps2; x2 *= ps4_to_ps2; x3 *= ps4_to_ps2;

        d0 += qdot16_hi2(((device const uint*)(rptr0 + off + 304u))[lane], x0, x1, x2, x3);
        d1 += qdot16_hi2(((device const uint*)(rptr1 + off + 304u))[lane], x0, x1, x2, x3);

        float sc0 = float(((device const half*)(rptr0 + off))[blk]) * float(((device const char*)(rptr0 + off + 16u))[lane]);
        float sc1 = float(((device const half*)(rptr1 + off))[blk]) * float(((device const char*)(rptr1 + off + 16u))[lane]);
        acc0 = fma(d0, sc0, acc0);
        acc1 = fma(d1, sc1, acc1);
    }

    acc0 = simd_sum(acc0);
    acc1 = simd_sum(acc1);

    if (lane == 0u) {
        device half* dst = y + token * p.N + row0;
        dst[0] = half(acc0);
        dst[1] = half(acc1);
    }
}

// ----------------------------------------------------------------------------
// 5. Two-Stage Bit-Plane LM-Head (25.8 MB 1-bit screen -> 6,720 exact Q6K_M rescore!)
//    Stage 1: Grid (1680, 1, 1), Threads 256 (8 SGs x 2 passes = 64 rows, 12KB LUT)
//    Stage 2: Grid (840, 1, 1), Threads 128 (exact Q6K_M on top 6,720 candidates)
//    Stage 3: Grid (1, 1, 1), Threads 256 (final argmax into GPU state buffer)
// ----------------------------------------------------------------------------
struct ScreenParams {
    uint check_stop;
};

[[max_total_threads_per_threadgroup(256)]]
kernel void lm_head_screen_1bit(
    device const uchar* screen_rows [[buffer(0)]],
    device const half4* x [[buffer(1)]],
    device const uint* valid_mask [[buffer(2)]],
    device uint* cand_ids [[buffer(3)]],
    device const uint* state [[buffer(4)]],
    constant ScreenParams& p [[buffer(5)]],
    uint tg_id [[threadgroup_position_in_grid]],
    uint tid [[thread_index_in_threadgroup]],
    uint sg_id [[simdgroup_index_in_threadgroup]],
    uint lane [[thread_index_in_simdgroup]]
) {
    if (p.check_stop != 0u && state[0] != 0u) {
        return;
    }

    threadgroup half lut[384 * 16];
    for (uint g = tid; g < 384u; g += 256u) {
        half4 xv = x[g];
        half a = xv.x, b = xv.y, c = xv.z, d = xv.w;
        threadgroup half* dst = lut + g * 16u;
        half p0 = -a - b, p1 = a - b, p2 = -a + b, p3 = a + b;
        half q0 = -c - d, q1 = c - d, q2 = -c + d, q3 = c + d;
        dst[ 0] = p0 + q0; dst[ 1] = p1 + q0; dst[ 2] = p2 + q0; dst[ 3] = p3 + q0;
        dst[ 4] = p0 + q1; dst[ 5] = p1 + q1; dst[ 6] = p2 + q1; dst[ 7] = p3 + q1;
        dst[ 8] = p0 + q2; dst[ 9] = p1 + q2; dst[10] = p2 + q2; dst[11] = p3 + q2;
        dst[12] = p0 + q3; dst[13] = p1 + q3; dst[14] = p2 + q3; dst[15] = p3 + q3;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const uint sub_row = lane >> 3;
    const uint sub = lane & 7u;
    const uint base_vocab = tg_id * 64u;
    threadgroup float tg_scores[64];

    for (uint pass = 0u; pass < 2u; ++pass) {
        uint local_r = pass * 32u + sg_id * 4u + sub_row;
        uint vocab_id = base_vocab + local_r;
        device const uchar* rp = screen_rows + vocab_id * 240u;
        device const half* sp = (device const half*)rp;
        device const uint2* bp = (device const uint2*)(rp + 48u);

        float acc = 0.0f;
        for (uint st = 0u; st < 3u; ++st) {
            uint g = st * 8u + sub;
            float sc = float(sp[g]);
            uint2 bits = bp[g];
            uint lb = g * 256u;
            half d0 = (lut[lb +   0u + ( bits.x        & 15u)]
                     + lut[lb +  16u + ((bits.x >>  4) & 15u)])
                    + (lut[lb +  32u + ((bits.x >>  8) & 15u)]
                     + lut[lb +  48u + ((bits.x >> 12) & 15u)]);
            half d1 = (lut[lb +  64u + ((bits.x >> 16) & 15u)]
                     + lut[lb +  80u + ((bits.x >> 20) & 15u)])
                    + (lut[lb +  96u + ((bits.x >> 24) & 15u)]
                     + lut[lb + 112u + ( bits.x >> 28        )]);
            half d2 = (lut[lb + 128u + ( bits.y        & 15u)]
                     + lut[lb + 144u + ((bits.y >>  4) & 15u)])
                    + (lut[lb + 160u + ((bits.y >>  8) & 15u)]
                     + lut[lb + 176u + ((bits.y >> 12) & 15u)]);
            half d3 = (lut[lb + 192u + ((bits.y >> 16) & 15u)]
                     + lut[lb + 208u + ((bits.y >> 20) & 15u)])
                    + (lut[lb + 224u + ((bits.y >> 24) & 15u)]
                     + lut[lb + 240u + ( bits.y >> 28        )]);
            acc = fma(float((d0 + d1) + (d2 + d3)), sc, acc);
        }
        acc += simd_shuffle_xor(acc, 4u);
        acc += simd_shuffle_xor(acc, 2u);
        acc += simd_shuffle_xor(acc, 1u);

        if (sub == 0u) {
            uint mw = valid_mask[vocab_id >> 5];
            bool ok = (((mw >> (vocab_id & 31u)) & 1u) != 0u) && isfinite(acc);
            tg_scores[local_r] = ok ? acc : -INFINITY;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (sg_id == 0u) {
        float v0 = tg_scores[lane];
        uint i0 = base_vocab + lane;
        float v1 = tg_scores[lane + 32u];
        uint i1 = base_vocab + lane + 32u;
        if (v1 > v0) { float tv = v0; v0 = v1; v1 = tv; uint ti = i0; i0 = i1; i1 = ti; }

        for (uint k = 0u; k < 4u; ++k) {
            float bk = v0;
            uint idk = i0;
            for (uint off = 16u; off > 0u; off >>= 1) {
                float ov = simd_shuffle_xor(bk, off);
                uint oi = simd_shuffle_xor(idk, off);
                if (ov > bk || (ov == bk && oi < idk)) { bk = ov; idk = oi; }
            }
            if (lane == 0u) {
                cand_ids[tg_id * 4u + k] = idk;
            }
            if (i0 == idk) {
                v0 = v1;
                i0 = i1;
                v1 = -INFINITY;
                i1 = 0xFFFFFFFFu;
            }
        }
    }
}

[[max_total_threads_per_threadgroup(128)]]
kernel void lm_head_rescore_q6k(
    device const uchar* w_sb [[buffer(0)]],
    device const half4* x [[buffer(1)]],
    device const uint* cand_ids [[buffer(2)]],
    device float* partial_val [[buffer(3)]],
    device uint* partial_id [[buffer(4)]],
    device const uint* state [[buffer(5)]],
    constant ScreenParams& p [[buffer(6)]],
    uint tg_id [[threadgroup_position_in_grid]],
    uint sg_id [[simdgroup_index_in_threadgroup]],
    uint lane [[thread_index_in_simdgroup]]
) {
    if (p.check_stop != 0u && state[0] != 0u) {
        return;
    }
    const uint half_id = lane >> 4;
    const uint sub = lane & 15u;
    const uint slot = sg_id * 2u + half_id;
    const uint vocab_id = cand_ids[tg_id * 8u + slot];
    const float4 ps4 = float4(1.0f, 1.0f / 16.0f, 1.0f / 256.0f, 1.0f / 4096.0f);
    const float4 ps4_to_ps2 = float4(16.0f, 64.0f, 256.0f, 1024.0f);
    const uint blk = sub >> 3;

    float acc = 0.0f;
    if (vocab_id < 107520u) {
        device const uchar* rp = w_sb + vocab_id * 1296u;
        for (uint sb = 0u; sb < 3u; ++sb) {
            uint off = sb * 432u;
            float df = float(((device const half*)(rp + off))[blk]);
            for (uint t = 0u; t < 2u; ++t) {
                uint g = sub * 2u + t;
                uint ix = (sb * 32u + g) * 4u;
                float4 x0 = float4(x[ix + 0u]); float4 x1 = float4(x[ix + 1u]);
                float4 x2 = float4(x[ix + 2u]); float4 x3 = float4(x[ix + 3u]);
                float4 sv = (x0 + x1) + (x2 + x3);
                float neg32 = -32.0f * ((sv.x + sv.y) + (sv.z + sv.w));
                x0 *= ps4; x1 *= ps4; x2 *= ps4; x3 *= ps4;
                float d = qdot16_u2(((device const uint2*)(rp + off + 48u))[g], x0, x1, x2, x3) + neg32;
                x0 *= ps4_to_ps2; x1 *= ps4_to_ps2; x2 *= ps4_to_ps2; x3 *= ps4_to_ps2;
                d += qdot16_hi2(((device const uint*)(rp + off + 304u))[g], x0, x1, x2, x3);
                float sc = df * float(((device const char*)(rp + off + 16u))[g]);
                acc = fma(d, sc, acc);
            }
        }
        for (uint off = 8u; off > 0u; off >>= 1) {
            acc += simd_shuffle_xor(acc, off);
        }
    } else {
        acc = -INFINITY;
    }

    threadgroup float tg_v[8];
    threadgroup uint tg_i[8];
    if (sub == 0u) {
        tg_v[slot] = isfinite(acc) ? acc : -INFINITY;
        tg_i[slot] = vocab_id;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (sg_id == 0u && lane == 0u) {
        float bv = tg_v[0];
        uint bi = tg_i[0];
        for (uint k = 1u; k < 8u; ++k) {
            if (tg_v[k] > bv || (tg_v[k] == bv && tg_i[k] < bi)) {
                bv = tg_v[k];
                bi = tg_i[k];
            }
        }
        partial_val[tg_id] = bv;
        partial_id[tg_id] = bi;
    }
}

[[max_total_threads_per_threadgroup(64)]]
kernel void lm_head_full_q6k(
    device const uchar* w_sb [[buffer(0)]],
    device const half4* x [[buffer(1)]],
    device float* full_logits [[buffer(2)]],
    uint tg_id [[threadgroup_position_in_grid]],
    uint sg_id [[simdgroup_index_in_threadgroup]],
    uint lane [[thread_index_in_simdgroup]]
) {
    const uint row0 = (tg_id * 2u + sg_id) * 2u;
    const float4 ps4 = float4(1.0f, 1.0f / 16.0f, 1.0f / 256.0f, 1.0f / 4096.0f);
    const float4 ps4_to_ps2 = float4(16.0f, 64.0f, 256.0f, 1024.0f);
    const uint blk = lane >> 4;

    device const uchar* rptr0 = w_sb + (row0 + 0u) * 1296u;
    device const uchar* rptr1 = w_sb + (row0 + 1u) * 1296u;

    float acc0 = 0.0f, acc1 = 0.0f;

    for (uint sb = 0u; sb < 3u; ++sb) {
        uint ix = (sb * 32u + lane) * 4u;
        float4 x0 = float4(x[ix + 0u]); float4 x1 = float4(x[ix + 1u]);
        float4 x2 = float4(x[ix + 2u]); float4 x3 = float4(x[ix + 3u]);
        float4 sv = (x0 + x1) + (x2 + x3);
        float neg32 = -32.0f * ((sv.x + sv.y) + (sv.z + sv.w));

        x0 *= ps4; x1 *= ps4; x2 *= ps4; x3 *= ps4;

        uint off = sb * 432u;
        float d0 = qdot16_u2(((device const uint2*)(rptr0 + off + 48u))[lane], x0, x1, x2, x3) + neg32;
        float d1 = qdot16_u2(((device const uint2*)(rptr1 + off + 48u))[lane], x0, x1, x2, x3) + neg32;

        x0 *= ps4_to_ps2; x1 *= ps4_to_ps2; x2 *= ps4_to_ps2; x3 *= ps4_to_ps2;

        d0 += qdot16_hi2(((device const uint*)(rptr0 + off + 304u))[lane], x0, x1, x2, x3);
        d1 += qdot16_hi2(((device const uint*)(rptr1 + off + 304u))[lane], x0, x1, x2, x3);

        float sc0 = float(((device const half*)(rptr0 + off))[blk]) * float(((device const char*)(rptr0 + off + 16u))[lane]);
        float sc1 = float(((device const half*)(rptr1 + off))[blk]) * float(((device const char*)(rptr1 + off + 16u))[lane]);
        acc0 = fma(d0, sc0, acc0);
        acc1 = fma(d1, sc1, acc1);
    }

    acc0 = simd_sum(acc0);
    acc1 = simd_sum(acc1);

    if (lane == 0u) {
        full_logits[row0 + 0u] = acc0;
        full_logits[row0 + 1u] = acc1;
    }
}

struct ArgmaxParams {
    uint num_blocks;
    uint step_out_slot;
    uint check_stop;
};

[[max_total_threads_per_threadgroup(256)]]
kernel void argmax_final(
    device const float* partial_val [[buffer(0)]],
    device const uint* partial_id [[buffer(1)]],
    device const uint* stop_mask [[buffer(2)]],
    device uint* state [[buffer(3)]],
    constant ArgmaxParams& p [[buffer(4)]],
    uint tid [[thread_index_in_threadgroup]],
    uint sg_id [[simdgroup_index_in_threadgroup]],
    uint lane [[thread_index_in_simdgroup]]
) {
    if (p.check_stop != 0u && state[0] != 0u) {
        return;
    }
    float bv = -INFINITY;
    uint bi = 0xFFFFFFFFu;
    for (uint i = tid; i < p.num_blocks; i += 256u) {
        float v = partial_val[i];
        uint id = partial_id[i];
        if (v > bv || (v == bv && id < bi)) {
            bv = v;
            bi = id;
        }
    }
    for (uint off = 16u; off > 0u; off >>= 1) {
        float ov = simd_shuffle_xor(bv, off);
        uint oi = simd_shuffle_xor(bi, off);
        if (ov > bv || (ov == bv && oi < bi)) {
            bv = ov;
            bi = oi;
        }
    }
    threadgroup float sg_v[8];
    threadgroup uint sg_i[8];
    if (lane == 0u) {
        sg_v[sg_id] = bv;
        sg_i[sg_id] = bi;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (tid == 0u) {
        float fv = sg_v[0];
        uint fi = sg_i[0];
        for (uint k = 1u; k < 8u; ++k) {
            if (sg_v[k] > fv || (sg_v[k] == fv && sg_i[k] < fi)) {
                fv = sg_v[k];
                fi = sg_i[k];
            }
        }
        state[2u + p.step_out_slot] = fi;
        state[1u] = p.step_out_slot;
        if (fi >= 107520u || ((stop_mask[fi >> 5] >> (fi & 31u)) & 1u) != 0u) {
            state[0u] = 1u;
        }
    }
}

// ----------------------------------------------------------------------------
// 6A. Prefill GEMM: Q4K_M (BM=64 and BM=32) with optional FUSED SwiGLU, FP16 I/O, FP32 acc
// ----------------------------------------------------------------------------
struct MatmulParams {
    uint M;
    uint N;
    uint K;
    uint fused_swiglu;
};

[[max_total_threads_per_threadgroup(128)]]
kernel void matmul_q4k_64x64(
    device const uchar* w_sb [[buffer(0)]],
    device const half* x [[buffer(1)]],
    device half* y [[buffer(2)]],
    constant MatmulParams& p [[buffer(3)]],
    uint2 tg_pos [[threadgroup_position_in_grid]],
    uint tid [[thread_index_in_threadgroup]],
    uint sg_id [[simdgroup_index_in_threadgroup]]
) {
    const uint n0 = tg_pos.x * 64u;
    const uint m0 = tg_pos.y * 64u;
    const uint M = p.M;
    const uint N = p.N;
    const uint K = p.K;
    const uint sg_m = sg_id >> 1;
    const uint sg_n = sg_id & 1u;

    threadgroup half shmem[4096];
    threadgroup half* As = shmem;
    threadgroup half* Bs = shmem + 2048;

    simdgroup_float8x8 acc[4][4];
    for (uint i = 0; i < 4; ++i)
        for (uint j = 0; j < 4; ++j)
            acc[i][j] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);

    const uint k_tiles = K >> 5;
    const uint row_bytes = (K >> 9) * 288u;
    const uint a_row0 = tid >> 3;
    const uint a_col4 = tid & 7u;
    const uint b_row = tid >> 1;
    const uint b_half = tid & 1u;

    device const uchar* row_ptr = w_sb + (n0 + b_row) * row_bytes;
    uint4 cur_hdr = uint4(0u);
    half2 cur_dm = half2(0.0h);

    for (uint kt = 0u; kt < k_tiles; ++kt) {
        const uint k_base = kt * 32u + a_col4 * 4u;
        for (uint r = 0u; r < 4u; ++r) {
            uint lr = a_row0 + r * 16u;
            uint gm = m0 + lr;
            half4 val = (gm < M) ? *(device const half4*)(x + gm * K + k_base) : half4(0.0h);
            *(threadgroup half4*)(As + lr * 32u + a_col4 * 4u) = val;
        }

        uint sb = kt >> 4;
        uint lt = kt & 15u;
        uint g = lt & 7u;
        device const uchar* sbp = row_ptr + sb * 288u;
        if (g == 0u) {
            cur_hdr = *(device const uint4*)(sbp + (lt >> 3) * 16u);
            cur_dm = as_type<half2>(cur_hdr.x);
        }
        uint shift = (g & 3u) * 8u;
        uint sy = (cur_hdr.y >> shift) & 0xFFu;
        uint sz = (cur_hdr.z >> shift) & 0xFFu;
        uint sw = (cur_hdr.w >> shift) & 0xFFu;
        bool lo = (g < 4u);
        uint sc6 = lo ? (sy & 63u) : ((sw & 15u) | ((sy >> 6) << 4));
        uint m6 = lo ? (sz & 63u) : ((sw >> 4) | ((sz >> 6) << 4));
        half sc = half(float(cur_dm.x) * float(sc6));
        half mn = half(-float(cur_dm.y) * float(m6));
        uint2 qw = ((device const uint2*)(sbp + 32u))[lt * 2u + b_half];
        ushort4 u = as_type<ushort4>(qw);

        threadgroup half4* b_dst = (threadgroup half4*)(Bs + b_row * 32u + b_half * 16u);
        b_dst[0] = fma(half4(u.x & 15u, (u.x >> 4) & 15u, (u.x >> 8) & 15u, u.x >> 12), sc, mn);
        b_dst[1] = fma(half4(u.y & 15u, (u.y >> 4) & 15u, (u.y >> 8) & 15u, u.y >> 12), sc, mn);
        b_dst[2] = fma(half4(u.z & 15u, (u.z >> 4) & 15u, (u.z >> 8) & 15u, u.z >> 12), sc, mn);
        b_dst[3] = fma(half4(u.w & 15u, (u.w >> 4) & 15u, (u.w >> 8) & 15u, u.w >> 12), sc, mn);

        threadgroup_barrier(mem_flags::mem_threadgroup);

        for (uint kk = 0u; kk < 32u; kk += 8u) {
            simdgroup_half8x8 mb[4];
            for (uint j = 0; j < 4; ++j)
                simdgroup_load(mb[j], Bs + (sg_n * 32u + j * 8u) * 32u + kk, 32u, ulong2(0, 0), true);
            for (uint i = 0; i < 4; ++i) {
                simdgroup_half8x8 ma;
                simdgroup_load(ma, As + (sg_m * 32u + i * 8u) * 32u + kk, 32u);
                for (uint j = 0; j < 4; ++j)
                    simdgroup_multiply_accumulate(acc[i][j], ma, mb[j], acc[i][j]);
            }
        }

        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    threadgroup float* stage = reinterpret_cast<threadgroup float*>(shmem);
    const uint s_row = tid >> 4;
    const uint s_col4 = tid & 15u;

    for (uint pass = 0u; pass < 2u; ++pass) {
        for (uint i = 0u; i < 2u; ++i) {
            for (uint j = 0u; j < 4u; ++j) {
                simdgroup_store(
                    acc[pass * 2u + i][j],
                    stage + (sg_m * 16u + i * 8u) * 64u + sg_n * 32u + j * 8u,
                    64u
                );
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        if (p.fused_swiglu != 0u) {
            const uint p_row = tid >> 3;
            const uint p_col4 = tid & 7u;
            const uint out_n = N >> 1;
            const uint out_n0 = n0 >> 1;
            for (uint r = 0u; r < 2u; ++r) {
                uint lr = p_row + r * 16u;
                uint gm = m0 + (lr >> 4) * 32u + pass * 16u + (lr & 15u);
                if (gm < M) {
                    float4 a = *(threadgroup float4*)(stage + lr * 64u + p_col4 * 8u);
                    float4 b = *(threadgroup float4*)(stage + lr * 64u + p_col4 * 8u + 4u);
                    float4 g = float4(a.x, a.z, b.x, b.z);
                    float4 u = float4(a.y, a.w, b.y, b.w);
                    *(device half4*)(y + gm * out_n + out_n0 + p_col4 * 4u) =
                        half4((g / (1.0f + exp(-g))) * u);
                }
            }
        } else {
            for (uint r = 0u; r < 4u; ++r) {
                uint lr = s_row + r * 8u;
                uint gm = m0 + (lr >> 4) * 32u + pass * 16u + (lr & 15u);
                if (gm < M) {
                    float4 v = *(threadgroup float4*)(stage + lr * 64u + s_col4 * 4u);
                    *(device half4*)(y + gm * N + n0 + s_col4 * 4u) = half4(v);
                }
            }
        }
        if (pass == 0u) {
            threadgroup_barrier(mem_flags::mem_threadgroup);
        }
    }
}

[[max_total_threads_per_threadgroup(128)]]
kernel void matmul_q4k_32x64(
    device const uchar* w_sb [[buffer(0)]],
    device const half* x [[buffer(1)]],
    device half* y [[buffer(2)]],
    constant MatmulParams& p [[buffer(3)]],
    uint2 tg_pos [[threadgroup_position_in_grid]],
    uint tid [[thread_index_in_threadgroup]],
    uint sg_id [[simdgroup_index_in_threadgroup]]
) {
    const uint n0 = tg_pos.x * 64u;
    const uint m0 = tg_pos.y * 32u;
    const uint M = p.M;
    const uint N = p.N;
    const uint K = p.K;
    const uint sg_m = sg_id >> 1;
    const uint sg_n = sg_id & 1u;

    threadgroup half shmem[4096];
    threadgroup half* As = shmem;
    threadgroup half* Bs = shmem + 1280;

    simdgroup_float8x8 acc[2][4];
    for (uint i = 0; i < 2; ++i)
        for (uint j = 0; j < 4; ++j)
            acc[i][j] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);

    const uint k_tiles = K >> 5;
    const uint row_bytes = (K >> 9) * 288u;
    const uint a_row0 = tid >> 3;
    const uint a_col4 = tid & 7u;
    const uint b_row = tid >> 1;
    const uint b_half = tid & 1u;

    device const uchar* row_ptr = w_sb + (n0 + b_row) * row_bytes;
    uint4 cur_hdr = uint4(0u);
    half2 cur_dm = half2(0.0h);

    for (uint kt = 0u; kt < k_tiles; ++kt) {
        const uint k_base = kt * 32u + a_col4 * 4u;
        for (uint r = 0u; r < 2u; ++r) {
            uint lr = a_row0 + r * 16u;
            uint gm = m0 + lr;
            half4 val = (gm < M) ? *(device const half4*)(x + gm * K + k_base) : half4(0.0h);
            *(threadgroup half4*)(As + lr * 40u + a_col4 * 4u) = val;
        }

        uint sb = kt >> 4;
        uint lt = kt & 15u;
        uint g = lt & 7u;
        device const uchar* sbp = row_ptr + sb * 288u;
        if (g == 0u) {
            cur_hdr = *(device const uint4*)(sbp + (lt >> 3) * 16u);
            cur_dm = as_type<half2>(cur_hdr.x);
        }
        uint shift = (g & 3u) * 8u;
        uint sy = (cur_hdr.y >> shift) & 0xFFu;
        uint sz = (cur_hdr.z >> shift) & 0xFFu;
        uint sw = (cur_hdr.w >> shift) & 0xFFu;
        bool lo = (g < 4u);
        uint sc6 = lo ? (sy & 63u) : ((sw & 15u) | ((sy >> 6) << 4));
        uint m6 = lo ? (sz & 63u) : ((sw >> 4) | ((sz >> 6) << 4));
        half sc = half(float(cur_dm.x) * float(sc6));
        half mn = half(-float(cur_dm.y) * float(m6));
        device const uint* qp = (device const uint*)(sbp + 32u) + lt * 4u + b_half * 2u;
        uint w0 = qp[0];
        uint w1 = qp[1];

        threadgroup half4* b_dst = (threadgroup half4*)(Bs + b_row * 40u + b_half * 16u);
        b_dst[0] = fma(half4(w0 & 15u, (w0 >> 4) & 15u, (w0 >> 8) & 15u, (w0 >> 12) & 15u), sc, mn);
        b_dst[1] = fma(half4((w0 >> 16) & 15u, (w0 >> 20) & 15u, (w0 >> 24) & 15u, w0 >> 28), sc, mn);
        b_dst[2] = fma(half4(w1 & 15u, (w1 >> 4) & 15u, (w1 >> 8) & 15u, (w1 >> 12) & 15u), sc, mn);
        b_dst[3] = fma(half4((w1 >> 16) & 15u, (w1 >> 20) & 15u, (w1 >> 24) & 15u, w1 >> 28), sc, mn);

        threadgroup_barrier(mem_flags::mem_threadgroup);

        for (uint kk = 0u; kk < 32u; kk += 8u) {
            simdgroup_half8x8 ma[2], mb[4];
            for (uint i = 0; i < 2; ++i)
                simdgroup_load(ma[i], As + (sg_m * 16u + i * 8u) * 40u + kk, 40u);
            for (uint j = 0; j < 4; ++j)
                simdgroup_load(mb[j], Bs + (sg_n * 32u + j * 8u) * 40u + kk, 40u, ulong2(0, 0), true);
            for (uint i = 0; i < 2; ++i)
                for (uint j = 0; j < 4; ++j)
                    simdgroup_multiply_accumulate(acc[i][j], ma[i], mb[j], acc[i][j]);
        }

        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    threadgroup float* stage = reinterpret_cast<threadgroup float*>(shmem);
    for (uint i = 0u; i < 2u; ++i) {
        for (uint j = 0u; j < 4u; ++j) {
            simdgroup_store(
                acc[i][j],
                stage + (sg_m * 16u + i * 8u) * 64u + sg_n * 32u + j * 8u,
                64u
            );
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (p.fused_swiglu != 0u) {
        const uint p_row = tid >> 3;
        const uint p_col4 = tid & 7u;
        const uint out_n = N >> 1;
        const uint out_n0 = n0 >> 1;
        for (uint r = 0u; r < 2u; ++r) {
            uint lr = p_row + r * 16u;
            uint gm = m0 + lr;
            if (gm < M) {
                float4 a = *(threadgroup float4*)(stage + lr * 64u + p_col4 * 8u);
                float4 b = *(threadgroup float4*)(stage + lr * 64u + p_col4 * 8u + 4u);
                float4 g = float4(a.x, a.z, b.x, b.z);
                float4 u = float4(a.y, a.w, b.y, b.w);
                *(device half4*)(y + gm * out_n + out_n0 + p_col4 * 4u) =
                    half4((g / (1.0f + exp(-g))) * u);
            }
        }
    } else {
        const uint s_row = tid >> 4;
        const uint s_col4 = tid & 15u;
        for (uint r = 0u; r < 4u; ++r) {
            uint lr = s_row + r * 8u;
            uint gm = m0 + lr;
            if (gm < M) {
                float4 v = *(threadgroup float4*)(stage + lr * 64u + s_col4 * 4u);
                *(device half4*)(y + gm * N + n0 + s_col4 * 4u) = half4(v);
            }
        }
    }
}

// ----------------------------------------------------------------------------
// 6B. Prefill GEMM: Q6K_M (BM=64 and BM=32), FP16 I/O, FP32 acc
// ----------------------------------------------------------------------------
static inline void unpack_q6k_16(device const uchar* sbp, uint g, threadgroup half4* dst) {
    float sc_f = float(((device const half*)sbp)[g >> 4]) * float(((device const char*)(sbp + 16u))[g]);
    half sc = half(sc_f);
    half neg32 = half(-32.0f * sc_f);
    uint2 ql = ((device const uint2*)(sbp + 48u))[g];
    uint qh = ((device const uint*)(sbp + 304u))[g];
    uint w0 = ql.x;
    uint w1 = ql.y;
    dst[0] = fma(half4(
        (w0 & 15u) | ((qh & 3u) << 4),
        ((w0 >> 4) & 15u) | (((qh >> 2) & 3u) << 4),
        ((w0 >> 8) & 15u) | (((qh >> 4) & 3u) << 4),
        ((w0 >> 12) & 15u) | (((qh >> 6) & 3u) << 4)
    ), sc, neg32);
    dst[1] = fma(half4(
        ((w0 >> 16) & 15u) | (((qh >> 8) & 3u) << 4),
        ((w0 >> 20) & 15u) | (((qh >> 10) & 3u) << 4),
        ((w0 >> 24) & 15u) | (((qh >> 12) & 3u) << 4),
        (w0 >> 28) | (((qh >> 14) & 3u) << 4)
    ), sc, neg32);
    dst[2] = fma(half4(
        (w1 & 15u) | (((qh >> 16) & 3u) << 4),
        ((w1 >> 4) & 15u) | (((qh >> 18) & 3u) << 4),
        ((w1 >> 8) & 15u) | (((qh >> 20) & 3u) << 4),
        ((w1 >> 12) & 15u) | (((qh >> 22) & 3u) << 4)
    ), sc, neg32);
    dst[3] = fma(half4(
        ((w1 >> 16) & 15u) | (((qh >> 24) & 3u) << 4),
        ((w1 >> 20) & 15u) | (((qh >> 26) & 3u) << 4),
        ((w1 >> 24) & 15u) | (((qh >> 28) & 3u) << 4),
        (w1 >> 28) | ((qh >> 30) << 4)
    ), sc, neg32);
}

[[max_total_threads_per_threadgroup(128)]]
kernel void matmul_q6k_64x64(
    device const uchar* w_sb [[buffer(0)]],
    device const half* x [[buffer(1)]],
    device half* y [[buffer(2)]],
    constant MatmulParams& p [[buffer(3)]],
    uint2 tg_pos [[threadgroup_position_in_grid]],
    uint tid [[thread_index_in_threadgroup]],
    uint sg_id [[simdgroup_index_in_threadgroup]]
) {
    const uint n0 = tg_pos.x * 64u;
    const uint m0 = tg_pos.y * 64u;
    const uint M = p.M;
    const uint N = p.N;
    const uint K = p.K;
    const uint sg_m = sg_id >> 1;
    const uint sg_n = sg_id & 1u;

    threadgroup half shmem[4096];
    threadgroup half* As = shmem;
    threadgroup half* Bs = shmem + 2048;

    simdgroup_float8x8 acc[4][4];
    for (uint i = 0; i < 4; ++i)
        for (uint j = 0; j < 4; ++j)
            acc[i][j] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);

    const uint k_tiles = K >> 5;
    const uint row_bytes = (K >> 9) * 432u;
    const uint a_row0 = tid >> 3;
    const uint a_col4 = tid & 7u;
    const uint b_row = tid >> 1;
    const uint b_half = tid & 1u;

    device const uchar* row_ptr = w_sb + (n0 + b_row) * row_bytes;

    for (uint kt = 0u; kt < k_tiles; ++kt) {
        const uint k_base = kt * 32u + a_col4 * 4u;
        for (uint r = 0u; r < 4u; ++r) {
            uint lr = a_row0 + r * 16u;
            uint gm = m0 + lr;
            half4 val = (gm < M) ? *(device const half4*)(x + gm * K + k_base) : half4(0.0h);
            *(threadgroup half4*)(As + lr * 32u + a_col4 * 4u) = val;
        }

        uint sb = kt >> 4;
        uint lt = kt & 15u;
        unpack_q6k_16(row_ptr + sb * 432u, lt * 2u + b_half, (threadgroup half4*)(Bs + b_row * 32u + b_half * 16u));

        threadgroup_barrier(mem_flags::mem_threadgroup);

        for (uint kk = 0u; kk < 32u; kk += 8u) {
            simdgroup_half8x8 mb[4];
            for (uint j = 0; j < 4; ++j)
                simdgroup_load(mb[j], Bs + (sg_n * 32u + j * 8u) * 32u + kk, 32u, ulong2(0, 0), true);
            for (uint i = 0; i < 4; ++i) {
                simdgroup_half8x8 ma;
                simdgroup_load(ma, As + (sg_m * 32u + i * 8u) * 32u + kk, 32u);
                for (uint j = 0; j < 4; ++j)
                    simdgroup_multiply_accumulate(acc[i][j], ma, mb[j], acc[i][j]);
            }
        }

        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    threadgroup float* stage = reinterpret_cast<threadgroup float*>(shmem);
    const uint s_row = tid >> 4;
    const uint s_col4 = tid & 15u;

    for (uint pass = 0u; pass < 2u; ++pass) {
        for (uint i = 0u; i < 2u; ++i) {
            for (uint j = 0u; j < 4u; ++j) {
                simdgroup_store(
                    acc[pass * 2u + i][j],
                    stage + (sg_m * 16u + i * 8u) * 64u + sg_n * 32u + j * 8u,
                    64u
                );
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        for (uint r = 0u; r < 4u; ++r) {
            uint lr = s_row + r * 8u;
            uint gm = m0 + (lr >> 4) * 32u + pass * 16u + (lr & 15u);
            if (gm < M) {
                float4 v = *(threadgroup float4*)(stage + lr * 64u + s_col4 * 4u);
                *(device half4*)(y + gm * N + n0 + s_col4 * 4u) = half4(v);
            }
        }
        if (pass == 0u) {
            threadgroup_barrier(mem_flags::mem_threadgroup);
        }
    }
}

[[max_total_threads_per_threadgroup(128)]]
kernel void matmul_q6k_32x64(
    device const uchar* w_sb [[buffer(0)]],
    device const half* x [[buffer(1)]],
    device half* y [[buffer(2)]],
    constant MatmulParams& p [[buffer(3)]],
    uint2 tg_pos [[threadgroup_position_in_grid]],
    uint tid [[thread_index_in_threadgroup]],
    uint sg_id [[simdgroup_index_in_threadgroup]]
) {
    const uint n0 = tg_pos.x * 64u;
    const uint m0 = tg_pos.y * 32u;
    const uint M = p.M;
    const uint N = p.N;
    const uint K = p.K;
    const uint sg_m = sg_id >> 1;
    const uint sg_n = sg_id & 1u;

    threadgroup half shmem[4096];
    threadgroup half* As = shmem;
    threadgroup half* Bs = shmem + 1280;

    simdgroup_float8x8 acc[2][4];
    for (uint i = 0; i < 2; ++i)
        for (uint j = 0; j < 4; ++j)
            acc[i][j] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);

    const uint k_tiles = K >> 5;
    const uint row_bytes = (K >> 9) * 432u;
    const uint a_row0 = tid >> 3;
    const uint a_col4 = tid & 7u;
    const uint b_row = tid >> 1;
    const uint b_half = tid & 1u;

    device const uchar* row_ptr = w_sb + (n0 + b_row) * row_bytes;

    for (uint kt = 0u; kt < k_tiles; ++kt) {
        const uint k_base = kt * 32u + a_col4 * 4u;
        for (uint r = 0u; r < 2u; ++r) {
            uint lr = a_row0 + r * 16u;
            uint gm = m0 + lr;
            half4 val = (gm < M) ? *(device const half4*)(x + gm * K + k_base) : half4(0.0h);
            *(threadgroup half4*)(As + lr * 40u + a_col4 * 4u) = val;
        }

        uint sb = kt >> 4;
        uint lt = kt & 15u;
        unpack_q6k_16(row_ptr + sb * 432u, lt * 2u + b_half, (threadgroup half4*)(Bs + b_row * 40u + b_half * 16u));

        threadgroup_barrier(mem_flags::mem_threadgroup);

        for (uint kk = 0u; kk < 32u; kk += 8u) {
            simdgroup_half8x8 ma[2], mb[4];
            for (uint i = 0; i < 2; ++i)
                simdgroup_load(ma[i], As + (sg_m * 16u + i * 8u) * 40u + kk, 40u);
            for (uint j = 0; j < 4; ++j)
                simdgroup_load(mb[j], Bs + (sg_n * 32u + j * 8u) * 40u + kk, 40u, ulong2(0, 0), true);
            for (uint i = 0; i < 2; ++i)
                for (uint j = 0; j < 4; ++j)
                    simdgroup_multiply_accumulate(acc[i][j], ma[i], mb[j], acc[i][j]);
        }

        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    threadgroup float* stage = reinterpret_cast<threadgroup float*>(shmem);
    for (uint i = 0u; i < 2u; ++i) {
        for (uint j = 0u; j < 4u; ++j) {
            simdgroup_store(
                acc[i][j],
                stage + (sg_m * 16u + i * 8u) * 64u + sg_n * 32u + j * 8u,
                64u
            );
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const uint s_row = tid >> 4;
    const uint s_col4 = tid & 15u;
    for (uint r = 0u; r < 4u; ++r) {
        uint lr = s_row + r * 8u;
        uint gm = m0 + lr;
        if (gm < M) {
            float4 v = *(threadgroup float4*)(stage + lr * 64u + s_col4 * 4u);
            *(device half4*)(y + gm * N + n0 + s_col4 * 4u) = half4(v);
        }
    }
}

// ----------------------------------------------------------------------------
// 7. Prefill MQA FlashAttention-2 (2 tokens x 16 heads = 32 Q rows, FP16 Q/O)
// ----------------------------------------------------------------------------
struct AttnPrefillParams {
    uint start_pos;
    uint count;
    uint full;
};

[[max_total_threads_per_threadgroup(128)]]
kernel void attention_prefill_mqa(
    device const half* q_buf [[buffer(0)]],
    device const half* keys [[buffer(1)]],
    device const half* values [[buffer(2)]],
    device half* out [[buffer(3)]],
    constant AttnPrefillParams& p [[buffer(4)]],
    uint tg_id [[threadgroup_position_in_grid]],
    uint tid [[thread_index_in_threadgroup]],
    uint sg_id [[simdgroup_index_in_threadgroup]],
    uint lane [[thread_index_in_simdgroup]]
) {
    const uint t0 = tg_id * 2u;
    if (t0 >= p.count) {
        return;
    }
    const uint rows = min(2u, p.count - t0) * 16u;
    const uint max_end = p.start_pos + min(t0 + 2u, p.count);
    const uint min_start = (p.full != 0u) ? 0u : ((p.start_pos + t0 + 1u > 2048u) ? (p.start_pos + t0 + 1u - 2048u) : 0u);

    threadgroup half Qs[32 * 128];
    threadgroup half Ks[32 * 128];
    threadgroup half Vs[32 * 128];
    threadgroup float S_stage[32 * 32];
    threadgroup half P_half[32 * 32];

    for (uint idx = tid; idx < 1024u; idx += 128u) {
        uint r = idx >> 5;
        uint c4 = idx & 31u;
        half4 v = (r < rows) ? *(device const half4*)(q_buf + (t0 * 16u + r) * 128u + c4 * 4u) : half4(0.0h);
        *(threadgroup half4*)(Qs + r * 128u + c4 * 4u) = v;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    simdgroup_float8x8 Oacc[16];
    for (uint d = 0u; d < 16u; ++d) {
        Oacc[d] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
    }

    const uint my_row = sg_id * 8u + (lane >> 2);
    const uint my_tok = t0 + (my_row >> 4);
    const uint my_end = p.start_pos + my_tok + 1u;
    const uint my_first = (p.full != 0u) ? 0u : ((my_end > 2048u) ? (my_end - 2048u) : 0u);

    threadgroup float row_m[32];
    threadgroup float row_l[32];
    threadgroup float row_alpha[32];
    if (tid < 32u) {
        row_m[tid] = -INFINITY;
        row_l[tid] = 0.0f;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    for (uint kv0 = min_start; kv0 < max_end; kv0 += 32u) {
        uint kv_len = min(32u, max_end - kv0);
        for (uint idx = tid; idx < 1024u; idx += 128u) {
            uint r = idx >> 5;
            uint c4 = idx & 31u;
            half4 kv = (r < kv_len) ? *(device const half4*)(keys + (kv0 + r) * 128u + c4 * 4u) : half4(0.0h);
            half4 vv = (r < kv_len) ? *(device const half4*)(values + (kv0 + r) * 128u + c4 * 4u) : half4(0.0h);
            *(threadgroup half4*)(Ks + r * 128u + c4 * 4u) = kv;
            *(threadgroup half4*)(Vs + r * 128u + c4 * 4u) = vv;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        simdgroup_float8x8 Sacc[4];
        for (uint j = 0u; j < 4u; ++j) {
            Sacc[j] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
        }
        for (uint d = 0u; d < 128u; d += 8u) {
            simdgroup_half8x8 mq;
            simdgroup_load(mq, Qs + sg_id * 8u * 128u + d, 128u);
            for (uint j = 0u; j < 4u; ++j) {
                simdgroup_half8x8 mk;
                simdgroup_load(mk, Ks + j * 8u * 128u + d, 128u, ulong2(0, 0), true);
                simdgroup_multiply_accumulate(Sacc[j], mq, mk, Sacc[j]);
            }
        }
        for (uint j = 0u; j < 4u; ++j) {
            simdgroup_store(Sacc[j], S_stage + sg_id * 8u * 32u + j * 8u, 32u);
        }
        simdgroup_barrier(mem_flags::mem_threadgroup);

        const uint sub = lane & 3u;
        float vals[8];
        float m_local = -INFINITY;
        for (uint k = 0u; k < 8u; ++k) {
            uint col = sub * 8u + k;
            uint kpos = kv0 + col;
            bool ok = (my_row < rows) && (col < kv_len) && (kpos >= my_first) && (kpos < my_end);
            float v = ok ? (S_stage[my_row * 32u + col] * ATTN_SCALE_LOG2E) : -INFINITY;
            vals[k] = v;
            m_local = max(m_local, v);
        }
        m_local = max(m_local, simd_shuffle_xor(m_local, 2u));
        m_local = max(m_local, simd_shuffle_xor(m_local, 1u));

        float old_m = row_m[my_row];
        float new_m = max(old_m, m_local);
        float alpha = (old_m == -INFINITY) ? 0.0f : exp2(old_m - new_m);
        float l_local = 0.0f;
        for (uint k = 0u; k < 8u; ++k) {
            float pval = (vals[k] == -INFINITY) ? 0.0f : exp2(vals[k] - new_m);
            P_half[my_row * 32u + sub * 8u + k] = half(pval);
            l_local += pval;
        }
        l_local += simd_shuffle_xor(l_local, 2u);
        l_local += simd_shuffle_xor(l_local, 1u);

        if (sub == 0u) {
            row_m[my_row] = new_m;
            row_l[my_row] = fma(row_l[my_row], alpha, l_local);
            row_alpha[my_row] = alpha;
        }
        simdgroup_barrier(mem_flags::mem_threadgroup);

        bool need_rescale = false;
        for (uint r = 0u; r < 8u; ++r) {
            if (row_alpha[sg_id * 8u + r] != 1.0f && row_alpha[sg_id * 8u + r] != 0.0f) {
                need_rescale = true;
            }
        }
        if (need_rescale) {
            for (uint d = 0u; d < 16u; d += 4u) {
                for (uint j = 0u; j < 4u; ++j) {
                    simdgroup_store(Oacc[d + j], S_stage + sg_id * 8u * 32u + j * 8u, 32u);
                }
                simdgroup_barrier(mem_flags::mem_threadgroup);
                float a_row = row_alpha[my_row];
                for (uint k = 0u; k < 8u; ++k) {
                    uint col = sub * 8u + k;
                    S_stage[my_row * 32u + col] *= a_row;
                }
                simdgroup_barrier(mem_flags::mem_threadgroup);
                for (uint j = 0u; j < 4u; ++j) {
                    simdgroup_load(Oacc[d + j], S_stage + sg_id * 8u * 32u + j * 8u, 32u);
                }
                simdgroup_barrier(mem_flags::mem_threadgroup);
            }
        }

        for (uint kk = 0u; kk < 32u; kk += 8u) {
            simdgroup_half8x8 mp;
            simdgroup_load(mp, P_half + sg_id * 8u * 32u + kk, 32u);
            for (uint d = 0u; d < 16u; ++d) {
                simdgroup_half8x8 mv;
                simdgroup_load(mv, Vs + kk * 128u + d * 8u, 128u);
                simdgroup_multiply_accumulate(Oacc[d], mp, mv, Oacc[d]);
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    const uint sub = lane & 3u;
    float inv_l = (row_l[my_row] > 0.0f) ? (1.0f / row_l[my_row]) : 0.0f;
    for (uint d = 0u; d < 16u; d += 4u) {
        for (uint j = 0u; j < 4u; ++j) {
            simdgroup_store(Oacc[d + j], S_stage + sg_id * 8u * 32u + j * 8u, 32u);
        }
        simdgroup_barrier(mem_flags::mem_threadgroup);
        if (my_row < rows) {
            uint base_out = (t0 * 16u + my_row) * 128u + d * 8u + sub * 8u;
            for (uint k = 0u; k < 8u; ++k) {
                out[base_out + k] = half(S_stage[my_row * 32u + sub * 8u + k] * inv_l);
            }
        }
        simdgroup_barrier(mem_flags::mem_threadgroup);
    }
}

// ----------------------------------------------------------------------------
// 8. Single-Pass Decode Attention (16 TGs x 256 threads = 8 SGs/head, FP16 Q/O)
// ----------------------------------------------------------------------------
struct AttnDecodeParams {
    uint pos;
    uint full;
    uint check_stop;
};

[[max_total_threads_per_threadgroup(256)]]
kernel void attention_decode_1pass(
    device const half* q_buf [[buffer(0)]],
    device const half* keys [[buffer(1)]],
    device const half* values [[buffer(2)]],
    device half* out [[buffer(3)]],
    device const uint* state [[buffer(4)]],
    constant AttnDecodeParams& p [[buffer(5)]],
    uint head [[threadgroup_position_in_grid]],
    uint tid [[thread_index_in_threadgroup]],
    uint sg_id [[simdgroup_index_in_threadgroup]],
    uint lane [[thread_index_in_simdgroup]]
) {
    if (p.check_stop != 0u && state[0] != 0u) {
        return;
    }
    const uint end = p.pos + 1u;
    const uint first = (p.full != 0u) ? 0u : ((end > 2048u) ? (end - 2048u) : 0u);
    const uint total = end - first;

    device const half4* q4 = (device const half4*)(q_buf + head * 128u);
    float4 qv = float4(q4[lane]) * ATTN_SCALE_LOG2E;

    float m = -INFINITY;
    float l = 0.0f;
    float4 o = float4(0.0f);

    uint idx = sg_id;
    for (; idx + 24u < total; idx += 32u) {
        uint p0 = first + idx;
        uint p1 = p0 + 8u;
        uint p2 = p0 + 16u;
        uint p3 = p0 + 24u;
        float s0 = simd_sum(dot(qv, float4(((device const half4*)(keys + p0 * 128u))[lane])));
        float s1 = simd_sum(dot(qv, float4(((device const half4*)(keys + p1 * 128u))[lane])));
        float s2 = simd_sum(dot(qv, float4(((device const half4*)(keys + p2 * 128u))[lane])));
        float s3 = simd_sum(dot(qv, float4(((device const half4*)(keys + p3 * 128u))[lane])));

        float m4 = max(max(s0, s1), max(s2, s3));
        float nm = max(m, m4);
        float alpha = (m == -INFINITY) ? 0.0f : exp2(m - nm);
        float w0 = exp2(s0 - nm);
        float w1 = exp2(s1 - nm);
        float w2 = exp2(s2 - nm);
        float w3 = exp2(s3 - nm);

        float4 v0 = float4(((device const half4*)(values + p0 * 128u))[lane]);
        float4 v1 = float4(((device const half4*)(values + p1 * 128u))[lane]);
        float4 v2 = float4(((device const half4*)(values + p2 * 128u))[lane]);
        float4 v3 = float4(((device const half4*)(values + p3 * 128u))[lane]);

        o = fma(o, alpha, fma(v0, w0, fma(v1, w1, fma(v2, w2, v3 * w3))));
        l = fma(l, alpha, (w0 + w1) + (w2 + w3));
        m = nm;
    }

    for (; idx < total; idx += 8u) {
        uint kpos = first + idx;
        float s = simd_sum(dot(qv, float4(((device const half4*)(keys + kpos * 128u))[lane])));
        float nm = max(m, s);
        float alpha = (m == -INFINITY) ? 0.0f : exp2(m - nm);
        float w = exp2(s - nm);
        o = fma(o, alpha, float4(((device const half4*)(values + kpos * 128u))[lane]) * w);
        l = fma(l, alpha, w);
        m = nm;
    }

    threadgroup float sg_m[8];
    threadgroup float sg_l[8];
    threadgroup float4 sg_o[8 * 32];
    if (lane == 0u) {
        sg_m[sg_id] = m;
        sg_l[sg_id] = l;
    }
    sg_o[sg_id * 32u + lane] = o;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (sg_id == 0u) {
        float gm = -INFINITY;
        for (uint s = 0u; s < 8u; ++s) {
            gm = max(gm, sg_m[s]);
        }
        float gl = 0.0f;
        float4 go = float4(0.0f);
        for (uint s = 0u; s < 8u; ++s) {
            if (sg_m[s] != -INFINITY) {
                float w = exp2(sg_m[s] - gm);
                gl = fma(sg_l[s], w, gl);
                go = fma(sg_o[s * 32u + lane], w, go);
            }
        }
        float inv = (gl > 0.0f) ? (1.0f / gl) : 0.0f;
        ((device half4*)(out + head * 128u))[lane] = half4(go * inv);
    }
}
