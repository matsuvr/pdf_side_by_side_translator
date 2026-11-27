// S4: 256-weight superblock containing 8 independent 32-weight scales.
// Symmetric weights: 4-bit biased nibbles; fp32 global scale and u8 local
// scales. At runtime scale multiplication and accumulation are integer until
// the entire superblock is complete. 4.375 bits per matrix weight.
use crate::{
    native,
    pool::Pool,
    weights::{Tensor, Weights},
};
use rayon::prelude::*;

const N: usize = 256;
const B: usize = 140;
// Same S4 quantization in output-major tiles, with no duplicated storage.
const TB: usize = B * 8;

pub fn quantize_matrix(raw: &[u8]) -> Vec<u8> {
    raw.par_chunks_exact(N * 2)
        .flat_map_iter(|b| {
            let raw: [f32; N] = std::array::from_fn(|i| native::bf16(&b[i * 2..]));
            let mut ds = [0.; 8];
            for g in 0..8 {
                let a = &raw[g * 32..g * 32 + 32];
                let max = a.iter().map(|x| x.abs()).fold(0., f32::max);
                let mut best = f32::INFINITY;
                let mut bestd = max / 7.;
                for r in [7., 7.5, 8., 8.5] {
                    let mut d = max / r;
                    if d > 0. {
                        for _ in 0..2 {
                            let (mut xy, mut yy) = (0., 0.);
                            for &x in a {
                                let q = (x / d).round_ties_even().clamp(-7., 7.);
                                xy += x * q;
                                yy += q * q;
                            }
                            if yy > 0. {
                                d = xy / yy;
                            }
                        }
                    }
                    let mse = a
                        .iter()
                        .map(|&x| {
                            let q = if d == 0. {
                                0.
                            } else {
                                (x / d).round_ties_even().clamp(-7., 7.)
                            };
                            (x - d * q).powi(2)
                        })
                        .sum::<f32>();
                    if mse < best {
                        best = mse;
                        bestd = d;
                    }
                }
                ds[g] = bestd;
            }
            let d = ds.iter().copied().fold(0., f32::max) / 127.;
            let mut o = [0u8; B];
            o[..4].copy_from_slice(&d.to_le_bytes());
            for g in 0..8 {
                let s = if d == 0. {
                    0
                } else {
                    (ds[g] / d).round_ties_even().clamp(1., 127.) as u8
                };
                o[4 + g] = s;
                let scale = d * s as f32;
                for i in 0..16 {
                    let q = |j: usize| {
                        if scale == 0. {
                            8
                        } else {
                            ((raw[g * 32 + j] / scale).round_ties_even().clamp(-7., 7.) as i32 + 8)
                                as u8
                        }
                    };
                    o[12 + 16 * g + i] = q(i) | (q(i + 16) << 4);
                }
            }
            o
        })
        .collect()
}

pub fn tile8(src: &[u8], n: usize, m: usize) -> Vec<u8> {
    let blocks = n / 256;
    let mut dst = vec![0; src.len()];
    assert_eq!(m % 8, 0);
    dst.par_chunks_exact_mut(blocks * TB)
        .enumerate()
        .for_each(|(tile, out)| {
            for b in 0..blocks {
                let d = &mut out[b * TB..(b + 1) * TB];
                for i in 0..8 {
                    let w = &src[((tile * 8 + i) * blocks + b) * B
                        ..((tile * 8 + i) * blocks + b + 1) * B];
                    d[i * 4..i * 4 + 4].copy_from_slice(&w[..4]);
                    for g in 0..8 {
                        d[32 + g * 8 + i] = w[4 + g];
                        for k in 0..4 {
                            let o = 96 + (g * 4 + k) * 32 + i * 4;
                            d[o..o + 4].copy_from_slice(&w[12 + g * 16 + k * 4..12 + g * 16 + k * 4 + 4]);
                        }
                    }
                }
            }
        });
    dst
}

#[repr(C, align(32))]
#[derive(Clone, Copy)]
pub(crate) struct Act {
    d: f32,
    neg_8_sums: [i32; 8],
    low: [i8; 128],
    high: [i8; 128],
}

impl Default for Act {
    fn default() -> Self {
        Self {
            d: 0.0,
            neg_8_sums: [0; 8],
            low: [0; 128],
            high: [0; 128],
        }
    }
}

#[cfg(target_arch = "x86_64")]
use std::arch::x86_64::*;

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn quantize_act_avx2(x: &[f32], dst: &mut Act) {
    debug_assert_eq!(x.len(), N);
    unsafe {
        let sign = _mm256_set1_ps(-0.0);
        let xp = x.as_ptr();
        let mut m0 = _mm256_setzero_ps();
        let mut m1 = _mm256_setzero_ps();
        let mut m2 = _mm256_setzero_ps();
        let mut m3 = _mm256_setzero_ps();
        for i in (0..N).step_by(32) {
            m0 = _mm256_max_ps(m0, _mm256_andnot_ps(sign, _mm256_loadu_ps(xp.add(i))));
            m1 = _mm256_max_ps(m1, _mm256_andnot_ps(sign, _mm256_loadu_ps(xp.add(i + 8))));
            m2 = _mm256_max_ps(m2, _mm256_andnot_ps(sign, _mm256_loadu_ps(xp.add(i + 16))));
            m3 = _mm256_max_ps(m3, _mm256_andnot_ps(sign, _mm256_loadu_ps(xp.add(i + 24))));
        }
        let mv = _mm256_max_ps(_mm256_max_ps(m0, m1), _mm256_max_ps(m2, m3));
        let lo = _mm256_castps256_ps128(mv);
        let hi = _mm256_extractf128_ps::<1>(mv);
        let m128 = _mm_max_ps(lo, hi);
        let m128 = _mm_max_ps(m128, _mm_movehl_ps(m128, m128));
        let m128 = _mm_max_ss(m128, _mm_shuffle_ps::<1>(m128, m128));
        let max_abs = _mm_cvtss_f32(m128);

        if max_abs == 0.0 {
            *dst = Act::default();
            return;
        }

        let d = max_abs / 127.0;
        dst.d = d;
        let v_d = _mm256_set1_ps(d);
        let ones_u8 = _mm256_set1_epi8(1);
        let ones_i16 = _mm256_set1_epi16(1);

        for g in 0..8 {
            let base = xp.add(g * 32);
            let q0 = _mm256_cvtps_epi32(_mm256_round_ps::<{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }>(
                _mm256_div_ps(_mm256_loadu_ps(base), v_d),
            ));
            let q1 = _mm256_cvtps_epi32(_mm256_round_ps::<{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }>(
                _mm256_div_ps(_mm256_loadu_ps(base.add(8)), v_d),
            ));
            let q2 = _mm256_cvtps_epi32(_mm256_round_ps::<{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }>(
                _mm256_div_ps(_mm256_loadu_ps(base.add(16)), v_d),
            ));
            let q3 = _mm256_cvtps_epi32(_mm256_round_ps::<{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }>(
                _mm256_div_ps(_mm256_loadu_ps(base.add(24)), v_d),
            ));

            let p0 = _mm256_permute4x64_epi64::<0xD8>(_mm256_packs_epi32(q0, q1));
            let p1 = _mm256_permute4x64_epi64::<0xD8>(_mm256_packs_epi32(q2, q3));
            let p8 = _mm256_permute4x64_epi64::<0xD8>(_mm256_packs_epi16(p0, p1));

            _mm_storeu_si128(
                dst.low.as_mut_ptr().add(g * 16).cast(),
                _mm256_castsi256_si128(p8),
            );
            _mm_storeu_si128(
                dst.high.as_mut_ptr().add(g * 16).cast(),
                _mm256_extracti128_si256::<1>(p8),
            );

            let s16 = _mm256_maddubs_epi16(ones_u8, p8);
            let s32 = _mm256_madd_epi16(s16, ones_i16);
            let s128 = _mm_add_epi32(_mm256_castsi256_si128(s32), _mm256_extracti128_si256::<1>(s32));
            let s128 = _mm_add_epi32(s128, _mm_srli_si128::<8>(s128));
            let s128 = _mm_add_epi32(s128, _mm_srli_si128::<4>(s128));
            dst.neg_8_sums[g] = -8 * _mm_cvtsi128_si32(s128);
        }
    }
}

fn quantize_act_scalar(x: &[f32], dst: &mut Act) {
    let max_abs = x.iter().map(|v| v.abs()).fold(0.0f32, f32::max);
    if max_abs == 0.0 {
        *dst = Act::default();
        return;
    }
    let d = max_abs / 127.0;
    let inv_d = 127.0 / max_abs;
    dst.d = d;
    for g in 0..8 {
        let mut sum = 0i32;
        for i in 0..16 {
            let ql = (x[g * 32 + i] * inv_d).round_ties_even().clamp(-127.0, 127.0) as i8;
            let qh = (x[g * 32 + 16 + i] * inv_d)
                .round_ties_even()
                .clamp(-127.0, 127.0) as i8;
            dst.low[g * 16 + i] = ql;
            dst.high[g * 16 + i] = qh;
            sum += ql as i32 + qh as i32;
        }
        dst.neg_8_sums[g] = -8 * sum;
    }
}

#[derive(Clone, Copy)]
enum Isa {
    #[cfg(target_arch = "x86_64")]
    Avx512Vnni,
    #[cfg(target_arch = "x86_64")]
    AvxVnniFma,
    #[cfg(target_arch = "x86_64")]
    Avx2Fma,
    #[cfg(target_arch = "x86_64")]
    Avx2,
    Scalar,
}

#[inline]
fn select_isa() -> Isa {
    #[cfg(target_arch = "x86_64")]
    {
        if std::is_x86_feature_detected!("avx2") {
            let fma = std::is_x86_feature_detected!("fma");
            if !cfg!(feature = "force-avx2")
                && fma
                && std::is_x86_feature_detected!("avx512vnni")
                && std::is_x86_feature_detected!("avx512vl")
            {
                return Isa::Avx512Vnni;
            }
            if !cfg!(feature = "force-avx2") && fma && std::is_x86_feature_detected!("avxvnni") {
                return Isa::AvxVnniFma;
            }
            if fma {
                return Isa::Avx2Fma;
            }
            return Isa::Avx2;
        }
    }
    Isa::Scalar
}

pub(crate) fn multiply_tile_pooled(
    pool: &Pool,
    g: &Weights,
    t: &Tensor,
    x: &[f32],
    out: &mut [f32],
    count: usize,
    acts: &mut Vec<Act>,
) {
    let n = t.dims[0];
    let m = t.dims[1];
    let blocks = n / 256;
    let total_blocks = count * blocks;
    let isa = select_isa();

    if acts.len() < total_blocks {
        acts.resize(total_blocks, Act::default());
    }
    let act_slice = &mut acts[..total_blocks];

    let use_avx2_quant = !matches!(isa, Isa::Scalar);
    if total_blocks >= 64 && pool.num_threads() > 1 {
        let act_addr = act_slice.as_mut_ptr() as usize;
        let nth = pool.num_threads().min(total_blocks / 16).max(1);
        pool.dispatch(nth, |ith, nth| {
            let b_start = ith * total_blocks / nth;
            let b_end = (ith + 1) * total_blocks / nth;
            let dst = unsafe {
                std::slice::from_raw_parts_mut((act_addr as *mut Act).add(b_start), b_end - b_start)
            };
            for (k, d) in dst.iter_mut().enumerate() {
                let blk = b_start + k;
                let src = &x[blk * N..(blk + 1) * N];
                #[cfg(target_arch = "x86_64")]
                if use_avx2_quant {
                    unsafe { quantize_act_avx2(src, d) };
                    continue;
                }
                quantize_act_scalar(src, d);
            }
        });
    } else {
        for (blk, d) in act_slice.iter_mut().enumerate() {
            let src = &x[blk * N..(blk + 1) * N];
            #[cfg(target_arch = "x86_64")]
            if use_avx2_quant {
                unsafe { quantize_act_avx2(src, d) };
                continue;
            }
            quantize_act_scalar(src, d);
        }
    }

    let data = g.data(t);
    let n_tiles = m / 8;
    let tile_bytes = blocks * TB;
    let out_addr = out.as_mut_ptr() as usize;
    let a: &[Act] = act_slice;

    if count == 1 {
        let nth = pool.decode_threads().min(n_tiles).max(1);
        pool.dispatch(nth, |ith, nth| {
            // Align thread boundaries to 2-tile (16-row) pairs when possible.
            let n_pairs = n_tiles / 2;
            let t_start = (ith * n_pairs / nth) * 2;
            let t_end = if ith + 1 == nth {
                n_tiles
            } else {
                ((ith + 1) * n_pairs / nth) * 2
            };
            if t_start >= t_end {
                return;
            }
            let out_ptr = out_addr as *mut f32;
            match isa {
                #[cfg(target_arch = "x86_64")]
                Isa::Avx512Vnni => unsafe {
                    run_decode_range_vnni(data, a, blocks, tile_bytes, t_start, t_end, out_ptr);
                },
                #[cfg(target_arch = "x86_64")]
                Isa::AvxVnniFma => unsafe {
                    run_decode_range_avxvnni(data, a, blocks, tile_bytes, t_start, t_end, out_ptr);
                },
                #[cfg(target_arch = "x86_64")]
                Isa::Avx2Fma => unsafe {
                    run_decode_range_fma(data, a, blocks, tile_bytes, t_start, t_end, out_ptr);
                },
                #[cfg(target_arch = "x86_64")]
                Isa::Avx2 => unsafe {
                    run_decode_range_avx2(data, a, blocks, tile_bytes, t_start, t_end, out_ptr);
                },
                Isa::Scalar => {
                    run_scalar_range(data, a, blocks, tile_bytes, t_start, t_end, out_ptr, m, 1);
                }
            }
        });
        return;
    }

    let nth = pool.num_threads().min(n_tiles).max(1);
    pool.dispatch(nth, |ith, nth| {
        let t_start = ith * n_tiles / nth;
        let t_end = (ith + 1) * n_tiles / nth;
        if t_start >= t_end {
            return;
        }
        let out_ptr = out_addr as *mut f32;
        match isa {
            #[cfg(target_arch = "x86_64")]
            Isa::Avx512Vnni => unsafe {
                for tile_idx in t_start..t_end {
                    let tile = &data[tile_idx * tile_bytes..(tile_idx + 1) * tile_bytes];
                    tile_vnni_many(tile, a, blocks, out_ptr.add(tile_idx * 8), m, count);
                }
            },
            #[cfg(target_arch = "x86_64")]
            Isa::AvxVnniFma => unsafe {
                for tile_idx in t_start..t_end {
                    let tile = &data[tile_idx * tile_bytes..(tile_idx + 1) * tile_bytes];
                    tile_avxvnni_many(tile, a, blocks, out_ptr.add(tile_idx * 8), m, count);
                }
            },
            #[cfg(target_arch = "x86_64")]
            Isa::Avx2Fma => unsafe {
                for tile_idx in t_start..t_end {
                    let tile = &data[tile_idx * tile_bytes..(tile_idx + 1) * tile_bytes];
                    tile_fma_many(tile, a, blocks, out_ptr.add(tile_idx * 8), m, count);
                }
            },
            #[cfg(target_arch = "x86_64")]
            Isa::Avx2 => unsafe {
                for tile_idx in t_start..t_end {
                    let tile = &data[tile_idx * tile_bytes..(tile_idx + 1) * tile_bytes];
                    tile_avx_many(tile, a, blocks, out_ptr.add(tile_idx * 8), m, count);
                }
            },
            Isa::Scalar => {
                run_scalar_range(data, a, blocks, tile_bytes, t_start, t_end, out_ptr, m, count);
            }
        }
    });
}

fn run_scalar_range(
    data: &[u8],
    a: &[Act],
    blocks: usize,
    tile_bytes: usize,
    t_start: usize,
    t_end: usize,
    out_ptr: *mut f32,
    m: usize,
    count: usize,
) {
    for tile_idx in t_start..t_end {
        let tile = &data[tile_idx * tile_bytes..(tile_idx + 1) * tile_bytes];
        for j in 0..count {
            let mut acc = [0.0f32; 8];
            for (b, w) in tile.chunks_exact(TB).enumerate() {
                let x = &a[j * blocks + b];
                for r in 0..8 {
                    let scale = f32::from_le_bytes(w[r * 4..r * 4 + 4].try_into().unwrap());
                    let mut sum = 0i32;
                    for g in 0..8 {
                        let mut s = x.neg_8_sums[g];
                        for k in 0..16 {
                            let v = w[96 + (g * 4 + k / 4) * 32 + r * 4 + k % 4];
                            s += (v & 15) as i32 * x.low[g * 16 + k] as i32
                                + (v >> 4) as i32 * x.high[g * 16 + k] as i32;
                        }
                        sum += s * w[32 + g * 8 + r] as i32;
                    }
                    acc[r] += sum as f32 * scale * x.d;
                }
            }
            unsafe {
                let dst = out_ptr.add(j * m + tile_idx * 8);
                std::ptr::copy_nonoverlapping(acc.as_ptr(), dst, 8);
            }
        }
    }
}

// ============================================================================
// Decode (count == 1): 2-tile (16-row) unrolled GEMV with 4 independent chains
// ============================================================================

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma,avxvnni")]
unsafe fn run_decode_range_avxvnni(
    data: &[u8],
    a: &[Act],
    blocks: usize,
    tile_bytes: usize,
    t_start: usize,
    t_end: usize,
    out_ptr: *mut f32,
) {
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let mut t = t_start;
        while t + 1 < t_end {
            let w0_base = data.as_ptr().add(t * tile_bytes);
            let w1_base = data.as_ptr().add((t + 1) * tile_bytes);
            let mut acc0 = _mm256_setzero_ps();
            let mut acc1 = _mm256_setzero_ps();

            for b in 0..blocks {
                let w0 = w0_base.add(b * TB);
                let w1 = w1_base.add(b * TB);
                if b + 1 < blocks {
                    _mm_prefetch::<_MM_HINT_T0>(w0.add(TB).cast());
                    _mm_prefetch::<_MM_HINT_T0>(w0.add(TB + 512).cast());
                    _mm_prefetch::<_MM_HINT_T0>(w1.add(TB).cast());
                    _mm_prefetch::<_MM_HINT_T0>(w1.add(TB + 512).cast());
                }
                let x = &a[b];
                let mut sum0 = _mm256_setzero_si256();
                let mut sum1 = _mm256_setzero_si256();

                for g in 0..8 {
                    let mut lo0 = _mm256_set1_epi32(x.neg_8_sums[g]);
                    let mut hi0 = _mm256_setzero_si256();
                    let mut lo1 = lo0;
                    let mut hi1 = _mm256_setzero_si256();

                    let lp = x.low.as_ptr().add(g * 16);
                    let hp = x.high.as_ptr().add(g * 16);
                    let q0_base = w0.add(96 + g * 128);
                    let q1_base = w1.add(96 + g * 128);

                    for k in 0..4 {
                        let al = _mm256_set1_epi32(std::ptr::read_unaligned(lp.add(k * 4).cast::<i32>()));
                        let ah = _mm256_set1_epi32(std::ptr::read_unaligned(hp.add(k * 4).cast::<i32>()));

                        let p0 = _mm256_loadu_si256(q0_base.add(k * 32).cast());
                        lo0 = _mm256_dpbusd_avx_epi32(lo0, _mm256_and_si256(p0, mask), al);
                        hi0 = _mm256_dpbusd_avx_epi32(
                            hi0,
                            _mm256_and_si256(_mm256_srli_epi16::<4>(p0), mask),
                            ah,
                        );

                        let p1 = _mm256_loadu_si256(q1_base.add(k * 32).cast());
                        lo1 = _mm256_dpbusd_avx_epi32(lo1, _mm256_and_si256(p1, mask), al);
                        hi1 = _mm256_dpbusd_avx_epi32(
                            hi1,
                            _mm256_and_si256(_mm256_srli_epi16::<4>(p1), mask),
                            ah,
                        );
                    }

                    let sc0 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w0.add(32 + g * 8).cast()));
                    let sc1 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w1.add(32 + g * 8).cast()));
                    sum0 = _mm256_add_epi32(sum0, _mm256_mullo_epi32(_mm256_add_epi32(lo0, hi0), sc0));
                    sum1 = _mm256_add_epi32(sum1, _mm256_mullo_epi32(_mm256_add_epi32(lo1, hi1), sc1));
                }

                let dw0 = _mm256_loadu_ps(w0.cast());
                let dw1 = _mm256_loadu_ps(w1.cast());
                let da = _mm256_set1_ps(x.d);
                acc0 = _mm256_fmadd_ps(_mm256_cvtepi32_ps(sum0), _mm256_mul_ps(dw0, da), acc0);
                acc1 = _mm256_fmadd_ps(_mm256_cvtepi32_ps(sum1), _mm256_mul_ps(dw1, da), acc1);
            }
            _mm256_storeu_ps(out_ptr.add(t * 8), acc0);
            _mm256_storeu_ps(out_ptr.add((t + 1) * 8), acc1);
            t += 2;
        }
        if t < t_end {
            let w0_base = data.as_ptr().add(t * tile_bytes);
            let mut acc0 = _mm256_setzero_ps();
            for b in 0..blocks {
                let w0 = w0_base.add(b * TB);
                let x = &a[b];
                let dw0 = _mm256_loadu_ps(w0.cast());
                let mut sum0 = _mm256_setzero_si256();
                for g in 0..8 {
                    let mut lo0 = _mm256_set1_epi32(x.neg_8_sums[g]);
                    let mut hi0 = _mm256_setzero_si256();
                    let lp = x.low.as_ptr().add(g * 16);
                    let hp = x.high.as_ptr().add(g * 16);
                    let q0_base = w0.add(96 + g * 128);
                    for k in 0..4 {
                        let al = _mm256_set1_epi32(std::ptr::read_unaligned(lp.add(k * 4).cast::<i32>()));
                        let ah = _mm256_set1_epi32(std::ptr::read_unaligned(hp.add(k * 4).cast::<i32>()));
                        let p0 = _mm256_loadu_si256(q0_base.add(k * 32).cast());
                        lo0 = _mm256_dpbusd_avx_epi32(lo0, _mm256_and_si256(p0, mask), al);
                        hi0 = _mm256_dpbusd_avx_epi32(
                            hi0,
                            _mm256_and_si256(_mm256_srli_epi16::<4>(p0), mask),
                            ah,
                        );
                    }
                    let sc0 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w0.add(32 + g * 8).cast()));
                    sum0 = _mm256_add_epi32(sum0, _mm256_mullo_epi32(_mm256_add_epi32(lo0, hi0), sc0));
                }
                acc0 = _mm256_fmadd_ps(
                    _mm256_cvtepi32_ps(sum0),
                    _mm256_mul_ps(dw0, _mm256_set1_ps(x.d)),
                    acc0,
                );
            }
            _mm256_storeu_ps(out_ptr.add(t * 8), acc0);
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma,avx512vnni,avx512vl")]
unsafe fn run_decode_range_vnni(
    data: &[u8],
    a: &[Act],
    blocks: usize,
    tile_bytes: usize,
    t_start: usize,
    t_end: usize,
    out_ptr: *mut f32,
) {
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let mut t = t_start;
        while t + 1 < t_end {
            let w0_base = data.as_ptr().add(t * tile_bytes);
            let w1_base = data.as_ptr().add((t + 1) * tile_bytes);
            let mut acc0 = _mm256_setzero_ps();
            let mut acc1 = _mm256_setzero_ps();

            for b in 0..blocks {
                let w0 = w0_base.add(b * TB);
                let w1 = w1_base.add(b * TB);
                if b + 1 < blocks {
                    _mm_prefetch::<_MM_HINT_T0>(w0.add(TB).cast());
                    _mm_prefetch::<_MM_HINT_T0>(w0.add(TB + 512).cast());
                    _mm_prefetch::<_MM_HINT_T0>(w1.add(TB).cast());
                    _mm_prefetch::<_MM_HINT_T0>(w1.add(TB + 512).cast());
                }
                let x = &a[b];
                let mut sum0 = _mm256_setzero_si256();
                let mut sum1 = _mm256_setzero_si256();

                for g in 0..8 {
                    let mut lo0 = _mm256_set1_epi32(x.neg_8_sums[g]);
                    let mut hi0 = _mm256_setzero_si256();
                    let mut lo1 = lo0;
                    let mut hi1 = _mm256_setzero_si256();

                    let lp = x.low.as_ptr().add(g * 16);
                    let hp = x.high.as_ptr().add(g * 16);
                    let q0_base = w0.add(96 + g * 128);
                    let q1_base = w1.add(96 + g * 128);

                    for k in 0..4 {
                        let al = _mm256_set1_epi32(std::ptr::read_unaligned(lp.add(k * 4).cast::<i32>()));
                        let ah = _mm256_set1_epi32(std::ptr::read_unaligned(hp.add(k * 4).cast::<i32>()));

                        let p0 = _mm256_loadu_si256(q0_base.add(k * 32).cast());
                        lo0 = _mm256_dpbusd_epi32(lo0, _mm256_and_si256(p0, mask), al);
                        hi0 = _mm256_dpbusd_epi32(
                            hi0,
                            _mm256_and_si256(_mm256_srli_epi16::<4>(p0), mask),
                            ah,
                        );

                        let p1 = _mm256_loadu_si256(q1_base.add(k * 32).cast());
                        lo1 = _mm256_dpbusd_epi32(lo1, _mm256_and_si256(p1, mask), al);
                        hi1 = _mm256_dpbusd_epi32(
                            hi1,
                            _mm256_and_si256(_mm256_srli_epi16::<4>(p1), mask),
                            ah,
                        );
                    }

                    let sc0 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w0.add(32 + g * 8).cast()));
                    let sc1 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w1.add(32 + g * 8).cast()));
                    sum0 = _mm256_add_epi32(sum0, _mm256_mullo_epi32(_mm256_add_epi32(lo0, hi0), sc0));
                    sum1 = _mm256_add_epi32(sum1, _mm256_mullo_epi32(_mm256_add_epi32(lo1, hi1), sc1));
                }

                let dw0 = _mm256_loadu_ps(w0.cast());
                let dw1 = _mm256_loadu_ps(w1.cast());
                let da = _mm256_set1_ps(x.d);
                acc0 = _mm256_fmadd_ps(_mm256_cvtepi32_ps(sum0), _mm256_mul_ps(dw0, da), acc0);
                acc1 = _mm256_fmadd_ps(_mm256_cvtepi32_ps(sum1), _mm256_mul_ps(dw1, da), acc1);
            }
            _mm256_storeu_ps(out_ptr.add(t * 8), acc0);
            _mm256_storeu_ps(out_ptr.add((t + 1) * 8), acc1);
            t += 2;
        }
        if t < t_end {
            let w0_base = data.as_ptr().add(t * tile_bytes);
            let mut acc0 = _mm256_setzero_ps();
            for b in 0..blocks {
                let w0 = w0_base.add(b * TB);
                let x = &a[b];
                let dw0 = _mm256_loadu_ps(w0.cast());
                let mut sum0 = _mm256_setzero_si256();
                for g in 0..8 {
                    let mut lo0 = _mm256_set1_epi32(x.neg_8_sums[g]);
                    let mut hi0 = _mm256_setzero_si256();
                    let lp = x.low.as_ptr().add(g * 16);
                    let hp = x.high.as_ptr().add(g * 16);
                    let q0_base = w0.add(96 + g * 128);
                    for k in 0..4 {
                        let al = _mm256_set1_epi32(std::ptr::read_unaligned(lp.add(k * 4).cast::<i32>()));
                        let ah = _mm256_set1_epi32(std::ptr::read_unaligned(hp.add(k * 4).cast::<i32>()));
                        let p0 = _mm256_loadu_si256(q0_base.add(k * 32).cast());
                        lo0 = _mm256_dpbusd_epi32(lo0, _mm256_and_si256(p0, mask), al);
                        hi0 = _mm256_dpbusd_epi32(
                            hi0,
                            _mm256_and_si256(_mm256_srli_epi16::<4>(p0), mask),
                            ah,
                        );
                    }
                    let sc0 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w0.add(32 + g * 8).cast()));
                    sum0 = _mm256_add_epi32(sum0, _mm256_mullo_epi32(_mm256_add_epi32(lo0, hi0), sc0));
                }
                acc0 = _mm256_fmadd_ps(
                    _mm256_cvtepi32_ps(sum0),
                    _mm256_mul_ps(dw0, _mm256_set1_ps(x.d)),
                    acc0,
                );
            }
            _mm256_storeu_ps(out_ptr.add(t * 8), acc0);
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
unsafe fn run_decode_range_fma(
    data: &[u8],
    a: &[Act],
    blocks: usize,
    tile_bytes: usize,
    t_start: usize,
    t_end: usize,
    out_ptr: *mut f32,
) {
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let ones = _mm256_set1_epi16(1);
        let mut t = t_start;
        while t + 1 < t_end {
            let w0_base = data.as_ptr().add(t * tile_bytes);
            let w1_base = data.as_ptr().add((t + 1) * tile_bytes);
            let mut acc0 = _mm256_setzero_ps();
            let mut acc1 = _mm256_setzero_ps();

            for b in 0..blocks {
                let w0 = w0_base.add(b * TB);
                let w1 = w1_base.add(b * TB);
                if b + 1 < blocks {
                    _mm_prefetch::<_MM_HINT_T0>(w0.add(TB).cast());
                    _mm_prefetch::<_MM_HINT_T0>(w0.add(TB + 512).cast());
                    _mm_prefetch::<_MM_HINT_T0>(w1.add(TB).cast());
                    _mm_prefetch::<_MM_HINT_T0>(w1.add(TB + 512).cast());
                }
                let x = &a[b];
                let mut sum0 = _mm256_setzero_si256();
                let mut sum1 = _mm256_setzero_si256();

                for g in 0..8 {
                    let mut pl0 = _mm256_setzero_si256();
                    let mut ph0 = _mm256_setzero_si256();
                    let mut pl1 = _mm256_setzero_si256();
                    let mut ph1 = _mm256_setzero_si256();

                    let lp = x.low.as_ptr().add(g * 16);
                    let hp = x.high.as_ptr().add(g * 16);
                    let q0_base = w0.add(96 + g * 128);
                    let q1_base = w1.add(96 + g * 128);

                    for k in 0..4 {
                        let al = _mm256_set1_epi32(std::ptr::read_unaligned(lp.add(k * 4).cast::<i32>()));
                        let ah = _mm256_set1_epi32(std::ptr::read_unaligned(hp.add(k * 4).cast::<i32>()));

                        let p0 = _mm256_loadu_si256(q0_base.add(k * 32).cast());
                        pl0 = _mm256_add_epi16(pl0, _mm256_maddubs_epi16(_mm256_and_si256(p0, mask), al));
                        ph0 = _mm256_add_epi16(
                            ph0,
                            _mm256_maddubs_epi16(_mm256_and_si256(_mm256_srli_epi16::<4>(p0), mask), ah),
                        );

                        let p1 = _mm256_loadu_si256(q1_base.add(k * 32).cast());
                        pl1 = _mm256_add_epi16(pl1, _mm256_maddubs_epi16(_mm256_and_si256(p1, mask), al));
                        ph1 = _mm256_add_epi16(
                            ph1,
                            _mm256_maddubs_epi16(_mm256_and_si256(_mm256_srli_epi16::<4>(p1), mask), ah),
                        );
                    }

                    let bias = _mm256_set1_epi32(x.neg_8_sums[g]);
                    let sc0 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w0.add(32 + g * 8).cast()));
                    let sc1 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w1.add(32 + g * 8).cast()));
                    let d0 = _mm256_add_epi32(_mm256_madd_epi16(_mm256_add_epi16(pl0, ph0), ones), bias);
                    let d1 = _mm256_add_epi32(_mm256_madd_epi16(_mm256_add_epi16(pl1, ph1), ones), bias);
                    sum0 = _mm256_add_epi32(sum0, _mm256_mullo_epi32(d0, sc0));
                    sum1 = _mm256_add_epi32(sum1, _mm256_mullo_epi32(d1, sc1));
                }

                let dw0 = _mm256_loadu_ps(w0.cast());
                let dw1 = _mm256_loadu_ps(w1.cast());
                let da = _mm256_set1_ps(x.d);
                acc0 = _mm256_fmadd_ps(_mm256_cvtepi32_ps(sum0), _mm256_mul_ps(dw0, da), acc0);
                acc1 = _mm256_fmadd_ps(_mm256_cvtepi32_ps(sum1), _mm256_mul_ps(dw1, da), acc1);
            }
            _mm256_storeu_ps(out_ptr.add(t * 8), acc0);
            _mm256_storeu_ps(out_ptr.add((t + 1) * 8), acc1);
            t += 2;
        }
        if t < t_end {
            let w0_base = data.as_ptr().add(t * tile_bytes);
            let mut acc0 = _mm256_setzero_ps();
            for b in 0..blocks {
                let w0 = w0_base.add(b * TB);
                let x = &a[b];
                let dw0 = _mm256_loadu_ps(w0.cast());
                let mut sum0 = _mm256_setzero_si256();
                for g in 0..8 {
                    let mut pl0 = _mm256_setzero_si256();
                    let mut ph0 = _mm256_setzero_si256();
                    let lp = x.low.as_ptr().add(g * 16);
                    let hp = x.high.as_ptr().add(g * 16);
                    let q0_base = w0.add(96 + g * 128);
                    for k in 0..4 {
                        let al = _mm256_set1_epi32(std::ptr::read_unaligned(lp.add(k * 4).cast::<i32>()));
                        let ah = _mm256_set1_epi32(std::ptr::read_unaligned(hp.add(k * 4).cast::<i32>()));
                        let p0 = _mm256_loadu_si256(q0_base.add(k * 32).cast());
                        pl0 = _mm256_add_epi16(pl0, _mm256_maddubs_epi16(_mm256_and_si256(p0, mask), al));
                        ph0 = _mm256_add_epi16(
                            ph0,
                            _mm256_maddubs_epi16(_mm256_and_si256(_mm256_srli_epi16::<4>(p0), mask), ah),
                        );
                    }
                    let sc0 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w0.add(32 + g * 8).cast()));
                    let d0 = _mm256_add_epi32(
                        _mm256_madd_epi16(_mm256_add_epi16(pl0, ph0), ones),
                        _mm256_set1_epi32(x.neg_8_sums[g]),
                    );
                    sum0 = _mm256_add_epi32(sum0, _mm256_mullo_epi32(d0, sc0));
                }
                acc0 = _mm256_fmadd_ps(
                    _mm256_cvtepi32_ps(sum0),
                    _mm256_mul_ps(dw0, _mm256_set1_ps(x.d)),
                    acc0,
                );
            }
            _mm256_storeu_ps(out_ptr.add(t * 8), acc0);
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn run_decode_range_avx2(
    data: &[u8],
    a: &[Act],
    blocks: usize,
    tile_bytes: usize,
    t_start: usize,
    t_end: usize,
    out_ptr: *mut f32,
) {
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let ones = _mm256_set1_epi16(1);
        for t in t_start..t_end {
            let w0_base = data.as_ptr().add(t * tile_bytes);
            let mut acc0 = _mm256_setzero_ps();
            for b in 0..blocks {
                let w0 = w0_base.add(b * TB);
                let x = &a[b];
                let dw0 = _mm256_loadu_ps(w0.cast());
                let mut sum0 = _mm256_setzero_si256();
                for g in 0..8 {
                    let mut pair = _mm256_setzero_si256();
                    for k in 0..4 {
                        let p0 = _mm256_loadu_si256(w0.add(96 + (g * 4 + k) * 32).cast());
                        let l = _mm256_and_si256(p0, mask);
                        let h = _mm256_and_si256(_mm256_srli_epi16::<4>(p0), mask);
                        let al = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.low.as_ptr().add(g * 16 + k * 4).cast::<i32>(),
                        ));
                        let ah = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.high.as_ptr().add(g * 16 + k * 4).cast::<i32>(),
                        ));
                        pair = _mm256_add_epi16(pair, _mm256_maddubs_epi16(l, al));
                        pair = _mm256_add_epi16(pair, _mm256_maddubs_epi16(h, ah));
                    }
                    let sc0 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w0.add(32 + g * 8).cast()));
                    let d0 = _mm256_add_epi32(
                        _mm256_madd_epi16(pair, ones),
                        _mm256_set1_epi32(x.neg_8_sums[g]),
                    );
                    sum0 = _mm256_add_epi32(sum0, _mm256_mullo_epi32(d0, sc0));
                }
                acc0 = _mm256_add_ps(
                    acc0,
                    _mm256_mul_ps(
                        _mm256_cvtepi32_ps(sum0),
                        _mm256_mul_ps(dw0, _mm256_set1_ps(x.d)),
                    ),
                );
            }
            _mm256_storeu_ps(out_ptr.add(t * 8), acc0);
        }
    }
}

// ============================================================================
// Prefill (count > 1): 8/4/2/1-token unrolled GEMM with direct strided stores
// ============================================================================

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma,avxvnni")]
unsafe fn tile_avxvnni_many(
    tile: &[u8],
    a: &[Act],
    blocks: usize,
    out_col: *mut f32,
    out_stride: usize,
    count: usize,
) {
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let mut base = 0;

        // 8 tokens at once: unpack each weight nibble ONCE across 8 tokens.
        while base + 8 <= count {
            let mut acc = [_mm256_setzero_ps(); 8];
            for (b, w) in tile.chunks_exact(TB).enumerate() {
                if b + 1 < blocks {
                    _mm_prefetch::<_MM_HINT_T0>(w.as_ptr().add(TB).cast());
                    _mm_prefetch::<_MM_HINT_T0>(w.as_ptr().add(TB + 512).cast());
                }
                let x: [&Act; 8] = std::array::from_fn(|j| &a[(base + j) * blocks + b]);
                let dw = _mm256_loadu_ps(w.as_ptr().cast());
                let mut sum = [_mm256_setzero_si256(); 8];

                for g in 0..8 {
                    let mut pair: [__m256i; 8] =
                        std::array::from_fn(|j| _mm256_set1_epi32(x[j].neg_8_sums[g]));
                    let q_base = w.as_ptr().add(96 + g * 128);
                    for k in 0..4 {
                        let v = _mm256_loadu_si256(q_base.add(k * 32).cast());
                        let l = _mm256_and_si256(v, mask);
                        let off = g * 16 + k * 4;
                        for j in 0..8 {
                            let al = _mm256_set1_epi32(std::ptr::read_unaligned(
                                x[j].low.as_ptr().add(off).cast::<i32>(),
                            ));
                            pair[j] = _mm256_dpbusd_avx_epi32(pair[j], l, al);
                        }
                        let h = _mm256_and_si256(_mm256_srli_epi16::<4>(v), mask);
                        for j in 0..8 {
                            let ah = _mm256_set1_epi32(std::ptr::read_unaligned(
                                x[j].high.as_ptr().add(off).cast::<i32>(),
                            ));
                            pair[j] = _mm256_dpbusd_avx_epi32(pair[j], h, ah);
                        }
                    }
                    let scale = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32 + g * 8).cast()));
                    for j in 0..8 {
                        sum[j] = _mm256_add_epi32(sum[j], _mm256_mullo_epi32(pair[j], scale));
                    }
                }
                for j in 0..8 {
                    acc[j] = _mm256_fmadd_ps(
                        _mm256_cvtepi32_ps(sum[j]),
                        _mm256_mul_ps(dw, _mm256_set1_ps(x[j].d)),
                        acc[j],
                    );
                }
            }
            for j in 0..8 {
                _mm256_storeu_ps(out_col.add((base + j) * out_stride), acc[j]);
            }
            base += 8;
        }

        while base + 4 <= count {
            let mut acc = [_mm256_setzero_ps(); 4];
            for (b, w) in tile.chunks_exact(TB).enumerate() {
                let x: [&Act; 4] = std::array::from_fn(|j| &a[(base + j) * blocks + b]);
                let dw = _mm256_loadu_ps(w.as_ptr().cast());
                let mut sum = [_mm256_setzero_si256(); 4];
                for g in 0..8 {
                    let mut pair: [__m256i; 4] =
                        std::array::from_fn(|j| _mm256_set1_epi32(x[j].neg_8_sums[g]));
                    let q_base = w.as_ptr().add(96 + g * 128);
                    for k in 0..4 {
                        let v = _mm256_loadu_si256(q_base.add(k * 32).cast());
                        let l = _mm256_and_si256(v, mask);
                        let h = _mm256_and_si256(_mm256_srli_epi16::<4>(v), mask);
                        let off = g * 16 + k * 4;
                        for j in 0..4 {
                            let al = _mm256_set1_epi32(std::ptr::read_unaligned(
                                x[j].low.as_ptr().add(off).cast::<i32>(),
                            ));
                            let ah = _mm256_set1_epi32(std::ptr::read_unaligned(
                                x[j].high.as_ptr().add(off).cast::<i32>(),
                            ));
                            pair[j] = _mm256_dpbusd_avx_epi32(pair[j], l, al);
                            pair[j] = _mm256_dpbusd_avx_epi32(pair[j], h, ah);
                        }
                    }
                    let scale = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32 + g * 8).cast()));
                    for j in 0..4 {
                        sum[j] = _mm256_add_epi32(sum[j], _mm256_mullo_epi32(pair[j], scale));
                    }
                }
                for j in 0..4 {
                    acc[j] = _mm256_fmadd_ps(
                        _mm256_cvtepi32_ps(sum[j]),
                        _mm256_mul_ps(dw, _mm256_set1_ps(x[j].d)),
                        acc[j],
                    );
                }
            }
            for j in 0..4 {
                _mm256_storeu_ps(out_col.add((base + j) * out_stride), acc[j]);
            }
            base += 4;
        }

        while base < count {
            let mut acc = _mm256_setzero_ps();
            for (b, w) in tile.chunks_exact(TB).enumerate() {
                let x = &a[base * blocks + b];
                let dw = _mm256_loadu_ps(w.as_ptr().cast());
                let mut sum = _mm256_setzero_si256();
                for g in 0..8 {
                    let mut lo = _mm256_set1_epi32(x.neg_8_sums[g]);
                    let mut hi = _mm256_setzero_si256();
                    let q_base = w.as_ptr().add(96 + g * 128);
                    for k in 0..4 {
                        let v = _mm256_loadu_si256(q_base.add(k * 32).cast());
                        let l = _mm256_and_si256(v, mask);
                        let h = _mm256_and_si256(_mm256_srli_epi16::<4>(v), mask);
                        let off = g * 16 + k * 4;
                        let al = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.low.as_ptr().add(off).cast::<i32>(),
                        ));
                        let ah = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.high.as_ptr().add(off).cast::<i32>(),
                        ));
                        lo = _mm256_dpbusd_avx_epi32(lo, l, al);
                        hi = _mm256_dpbusd_avx_epi32(hi, h, ah);
                    }
                    let scale = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32 + g * 8).cast()));
                    sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(_mm256_add_epi32(lo, hi), scale));
                }
                acc = _mm256_fmadd_ps(
                    _mm256_cvtepi32_ps(sum),
                    _mm256_mul_ps(dw, _mm256_set1_ps(x.d)),
                    acc,
                );
            }
            _mm256_storeu_ps(out_col.add(base * out_stride), acc);
            base += 1;
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma,avx512vnni,avx512vl")]
unsafe fn tile_vnni_many(
    tile: &[u8],
    a: &[Act],
    blocks: usize,
    out_col: *mut f32,
    out_stride: usize,
    count: usize,
) {
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let mut base = 0;

        while base + 8 <= count {
            let mut acc = [_mm256_setzero_ps(); 8];
            for (b, w) in tile.chunks_exact(TB).enumerate() {
                if b + 1 < blocks {
                    _mm_prefetch::<_MM_HINT_T0>(w.as_ptr().add(TB).cast());
                    _mm_prefetch::<_MM_HINT_T0>(w.as_ptr().add(TB + 512).cast());
                }
                let x: [&Act; 8] = std::array::from_fn(|j| &a[(base + j) * blocks + b]);
                let dw = _mm256_loadu_ps(w.as_ptr().cast());
                let mut sum = [_mm256_setzero_si256(); 8];
                for g in 0..8 {
                    let mut pair: [__m256i; 8] =
                        std::array::from_fn(|j| _mm256_set1_epi32(x[j].neg_8_sums[g]));
                    let q_base = w.as_ptr().add(96 + g * 128);
                    for k in 0..4 {
                        let v = _mm256_loadu_si256(q_base.add(k * 32).cast());
                        let l = _mm256_and_si256(v, mask);
                        let h = _mm256_and_si256(_mm256_srli_epi16::<4>(v), mask);
                        let off = g * 16 + k * 4;
                        for j in 0..8 {
                            let al = _mm256_set1_epi32(std::ptr::read_unaligned(
                                x[j].low.as_ptr().add(off).cast::<i32>(),
                            ));
                            pair[j] = _mm256_dpbusd_epi32(pair[j], l, al);
                        }
                        for j in 0..8 {
                            let ah = _mm256_set1_epi32(std::ptr::read_unaligned(
                                x[j].high.as_ptr().add(off).cast::<i32>(),
                            ));
                            pair[j] = _mm256_dpbusd_epi32(pair[j], h, ah);
                        }
                    }
                    let scale = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32 + g * 8).cast()));
                    for j in 0..8 {
                        sum[j] = _mm256_add_epi32(sum[j], _mm256_mullo_epi32(pair[j], scale));
                    }
                }
                for j in 0..8 {
                    acc[j] = _mm256_fmadd_ps(
                        _mm256_cvtepi32_ps(sum[j]),
                        _mm256_mul_ps(dw, _mm256_set1_ps(x[j].d)),
                        acc[j],
                    );
                }
            }
            for j in 0..8 {
                _mm256_storeu_ps(out_col.add((base + j) * out_stride), acc[j]);
            }
            base += 8;
        }

        while base < count {
            let mut acc = _mm256_setzero_ps();
            for (b, w) in tile.chunks_exact(TB).enumerate() {
                let x = &a[base * blocks + b];
                let dw = _mm256_loadu_ps(w.as_ptr().cast());
                let mut sum = _mm256_setzero_si256();
                for g in 0..8 {
                    let mut lo = _mm256_set1_epi32(x.neg_8_sums[g]);
                    let mut hi = _mm256_setzero_si256();
                    let q_base = w.as_ptr().add(96 + g * 128);
                    for k in 0..4 {
                        let v = _mm256_loadu_si256(q_base.add(k * 32).cast());
                        let l = _mm256_and_si256(v, mask);
                        let h = _mm256_and_si256(_mm256_srli_epi16::<4>(v), mask);
                        let off = g * 16 + k * 4;
                        let al = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.low.as_ptr().add(off).cast::<i32>(),
                        ));
                        let ah = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.high.as_ptr().add(off).cast::<i32>(),
                        ));
                        lo = _mm256_dpbusd_epi32(lo, l, al);
                        hi = _mm256_dpbusd_epi32(hi, h, ah);
                    }
                    let scale = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32 + g * 8).cast()));
                    sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(_mm256_add_epi32(lo, hi), scale));
                }
                acc = _mm256_fmadd_ps(
                    _mm256_cvtepi32_ps(sum),
                    _mm256_mul_ps(dw, _mm256_set1_ps(x.d)),
                    acc,
                );
            }
            _mm256_storeu_ps(out_col.add(base * out_stride), acc);
            base += 1;
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
unsafe fn tile_fma_many(
    tile: &[u8],
    a: &[Act],
    blocks: usize,
    out_col: *mut f32,
    out_stride: usize,
    count: usize,
) {
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let ones = _mm256_set1_epi16(1);
        let mut base = 0;

        while base + 4 <= count {
            let mut acc = [_mm256_setzero_ps(); 4];
            for (b, w) in tile.chunks_exact(TB).enumerate() {
                if b + 1 < blocks {
                    _mm_prefetch::<_MM_HINT_T0>(w.as_ptr().add(TB).cast());
                    _mm_prefetch::<_MM_HINT_T0>(w.as_ptr().add(TB + 512).cast());
                }
                let x: [&Act; 4] = std::array::from_fn(|j| &a[(base + j) * blocks + b]);
                let dw = _mm256_loadu_ps(w.as_ptr().cast());
                let mut sum = [_mm256_setzero_si256(); 4];
                for g in 0..8 {
                    let mut pair = [_mm256_setzero_si256(); 4];
                    let q_base = w.as_ptr().add(96 + g * 128);
                    for k in 0..4 {
                        let v = _mm256_loadu_si256(q_base.add(k * 32).cast());
                        let l = _mm256_and_si256(v, mask);
                        let h = _mm256_and_si256(_mm256_srli_epi16::<4>(v), mask);
                        let off = g * 16 + k * 4;
                        for j in 0..4 {
                            let al = _mm256_set1_epi32(std::ptr::read_unaligned(
                                x[j].low.as_ptr().add(off).cast::<i32>(),
                            ));
                            let ah = _mm256_set1_epi32(std::ptr::read_unaligned(
                                x[j].high.as_ptr().add(off).cast::<i32>(),
                            ));
                            pair[j] = _mm256_add_epi16(pair[j], _mm256_maddubs_epi16(l, al));
                            pair[j] = _mm256_add_epi16(pair[j], _mm256_maddubs_epi16(h, ah));
                        }
                    }
                    let scale = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32 + g * 8).cast()));
                    for j in 0..4 {
                        let s = _mm256_add_epi32(
                            _mm256_madd_epi16(pair[j], ones),
                            _mm256_set1_epi32(x[j].neg_8_sums[g]),
                        );
                        sum[j] = _mm256_add_epi32(sum[j], _mm256_mullo_epi32(s, scale));
                    }
                }
                for j in 0..4 {
                    acc[j] = _mm256_fmadd_ps(
                        _mm256_cvtepi32_ps(sum[j]),
                        _mm256_mul_ps(dw, _mm256_set1_ps(x[j].d)),
                        acc[j],
                    );
                }
            }
            for j in 0..4 {
                _mm256_storeu_ps(out_col.add((base + j) * out_stride), acc[j]);
            }
            base += 4;
        }

        while base < count {
            let mut acc = _mm256_setzero_ps();
            for (b, w) in tile.chunks_exact(TB).enumerate() {
                let x = &a[base * blocks + b];
                let dw = _mm256_loadu_ps(w.as_ptr().cast());
                let mut sum = _mm256_setzero_si256();
                for g in 0..8 {
                    let mut pl = _mm256_setzero_si256();
                    let mut ph = _mm256_setzero_si256();
                    let q_base = w.as_ptr().add(96 + g * 128);
                    for k in 0..4 {
                        let v = _mm256_loadu_si256(q_base.add(k * 32).cast());
                        let l = _mm256_and_si256(v, mask);
                        let h = _mm256_and_si256(_mm256_srli_epi16::<4>(v), mask);
                        let off = g * 16 + k * 4;
                        let al = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.low.as_ptr().add(off).cast::<i32>(),
                        ));
                        let ah = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.high.as_ptr().add(off).cast::<i32>(),
                        ));
                        pl = _mm256_add_epi16(pl, _mm256_maddubs_epi16(l, al));
                        ph = _mm256_add_epi16(ph, _mm256_maddubs_epi16(h, ah));
                    }
                    let scale = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32 + g * 8).cast()));
                    let dot = _mm256_add_epi32(
                        _mm256_madd_epi16(_mm256_add_epi16(pl, ph), ones),
                        _mm256_set1_epi32(x.neg_8_sums[g]),
                    );
                    sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(dot, scale));
                }
                acc = _mm256_fmadd_ps(
                    _mm256_cvtepi32_ps(sum),
                    _mm256_mul_ps(dw, _mm256_set1_ps(x.d)),
                    acc,
                );
            }
            _mm256_storeu_ps(out_col.add(base * out_stride), acc);
            base += 1;
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn tile_avx_many(
    tile: &[u8],
    a: &[Act],
    blocks: usize,
    out_col: *mut f32,
    out_stride: usize,
    count: usize,
) {
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let ones = _mm256_set1_epi16(1);
        let mut base = 0;
        while base + 2 <= count {
            let mut acc = [_mm256_setzero_ps(); 2];
            for (b, w) in tile.chunks_exact(TB).enumerate() {
                let x = [&a[base * blocks + b], &a[(base + 1) * blocks + b]];
                let dw = _mm256_loadu_ps(w.as_ptr().cast());
                let mut sum = [_mm256_setzero_si256(); 2];
                for g in 0..8 {
                    let mut pair = [_mm256_setzero_si256(); 2];
                    for k in 0..4 {
                        let v = _mm256_loadu_si256(w.as_ptr().add(96 + (g * 4 + k) * 32).cast());
                        let l = _mm256_and_si256(v, mask);
                        let h = _mm256_and_si256(_mm256_srli_epi16::<4>(v), mask);
                        for j in 0..2 {
                            let al = _mm256_set1_epi32(std::ptr::read_unaligned(
                                x[j].low.as_ptr().add(g * 16 + k * 4).cast::<i32>(),
                            ));
                            let ah = _mm256_set1_epi32(std::ptr::read_unaligned(
                                x[j].high.as_ptr().add(g * 16 + k * 4).cast::<i32>(),
                            ));
                            pair[j] = _mm256_add_epi16(pair[j], _mm256_maddubs_epi16(l, al));
                            pair[j] = _mm256_add_epi16(pair[j], _mm256_maddubs_epi16(h, ah));
                        }
                    }
                    let scale = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32 + g * 8).cast()));
                    for j in 0..2 {
                        let s = _mm256_add_epi32(
                            _mm256_madd_epi16(pair[j], ones),
                            _mm256_set1_epi32(x[j].neg_8_sums[g]),
                        );
                        sum[j] = _mm256_add_epi32(sum[j], _mm256_mullo_epi32(s, scale));
                    }
                }
                for j in 0..2 {
                    acc[j] = _mm256_add_ps(
                        acc[j],
                        _mm256_mul_ps(
                            _mm256_cvtepi32_ps(sum[j]),
                            _mm256_mul_ps(dw, _mm256_set1_ps(x[j].d)),
                        ),
                    );
                }
            }
            for j in 0..2 {
                _mm256_storeu_ps(out_col.add((base + j) * out_stride), acc[j]);
            }
            base += 2;
        }
        if base < count {
            let mut acc = _mm256_setzero_ps();
            for (b, w) in tile.chunks_exact(TB).enumerate() {
                let x = &a[base * blocks + b];
                let dw = _mm256_loadu_ps(w.as_ptr().cast());
                let mut sum = _mm256_setzero_si256();
                for g in 0..8 {
                    let mut pair = _mm256_setzero_si256();
                    for k in 0..4 {
                        let p = _mm256_loadu_si256(w.as_ptr().add(96 + (g * 4 + k) * 32).cast());
                        let l = _mm256_and_si256(p, mask);
                        let h = _mm256_and_si256(_mm256_srli_epi16::<4>(p), mask);
                        let al = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.low.as_ptr().add(g * 16 + k * 4).cast::<i32>(),
                        ));
                        let ah = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.high.as_ptr().add(g * 16 + k * 4).cast::<i32>(),
                        ));
                        pair = _mm256_add_epi16(pair, _mm256_maddubs_epi16(l, al));
                        pair = _mm256_add_epi16(pair, _mm256_maddubs_epi16(h, ah));
                    }
                    let scale = _mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32 + g * 8).cast()));
                    let dot = _mm256_add_epi32(
                        _mm256_madd_epi16(pair, ones),
                        _mm256_set1_epi32(x.neg_8_sums[g]),
                    );
                    sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(dot, scale));
                }
                acc = _mm256_add_ps(
                    acc,
                    _mm256_mul_ps(
                        _mm256_cvtepi32_ps(sum),
                        _mm256_mul_ps(dw, _mm256_set1_ps(x.d)),
                    ),
                );
            }
            _mm256_storeu_ps(out_col.add(base * out_stride), acc);
        }
    }
}

#[cfg(test)]
fn dot_scalar(row: &[u8], a: &[Act]) -> f32 {
    row.chunks_exact(B)
        .zip(a)
        .map(|(b, x)| {
            let dw = f32::from_le_bytes(b[..4].try_into().unwrap());
            let sum = (0..8)
                .map(|g| {
                    let s = (0..16)
                        .map(|i| {
                            let v = b[12 + g * 16 + i];
                            ((v & 15) as i32 - 8) * x.low[g * 16 + i] as i32
                                + ((v >> 4) as i32 - 8) * x.high[g * 16 + i] as i32
                        })
                        .sum::<i32>();
                    s * b[4 + g] as i32
                })
                .sum::<i32>();
            sum as f32 * dw * x.d
        })
        .sum()
}

#[cfg(test)]
#[test]
fn test_tile() {
    let raw = (0..512 * 16)
        .flat_map(|i| (((i as f32 * 0.731).sin().to_bits() >> 16) as u16).to_le_bytes())
        .collect::<Vec<_>>();
    let row = quantize_matrix(&raw);
    let tile = tile8(&row, 512, 16);
    let xs = (0..13)
        .flat_map(|j| (0..512).map(move |i| ((i + j * 37) as f32 * 0.123).cos()))
        .collect::<Vec<_>>();
    let mut a = vec![Act::default(); 26];
    for (idx, dst) in a.iter_mut().enumerate() {
        quantize_act_scalar(&xs[idx * 256..(idx + 1) * 256], dst);
    }
    let want: Vec<f32> = a
        .chunks_exact(2)
        .flat_map(|ab| row.chunks_exact(280).map(move |w| dot_scalar(w, ab)))
        .collect();

    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2") {
        // Test single-token 2-tile decode kernels
        let mut got1 = [0.0f32; 16];
        unsafe { run_decode_range_avx2(&tile, &a[..2], 2, 2 * TB, 0, 2, got1.as_mut_ptr()) };
        for (x, y) in got1.iter().zip(&want[..16]) {
            assert!((x - y).abs() < 1e-4, "avx2 decode {x} {y}");
        }
        if std::is_x86_feature_detected!("fma") {
            unsafe { run_decode_range_fma(&tile, &a[..2], 2, 2 * TB, 0, 2, got1.as_mut_ptr()) };
            for (x, y) in got1.iter().zip(&want[..16]) {
                assert!((x - y).abs() < 1e-4, "fma decode {x} {y}");
            }
            if std::is_x86_feature_detected!("avxvnni") {
                unsafe {
                    run_decode_range_avxvnni(&tile, &a[..2], 2, 2 * TB, 0, 2, got1.as_mut_ptr())
                };
                for (x, y) in got1.iter().zip(&want[..16]) {
                    assert!((x - y).abs() < 1e-4, "avxvnni decode {x} {y}");
                }
            }
        }

        // Test multi-token (13 tokens = 8 + 4 + 1) prefill kernels
        let mut got = vec![0.0f32; 13 * 16];
        for t in 0..2 {
            let sl = &tile[t * 2 * TB..(t + 1) * 2 * TB];
            unsafe { tile_avx_many(sl, &a, 2, got.as_mut_ptr().add(t * 8), 16, 13) };
        }
        for (x, y) in got.iter().zip(&want) {
            assert!((x - y).abs() < 1e-4, "avx2 many {x} {y}");
        }
        if std::is_x86_feature_detected!("fma") {
            got.fill(0.0);
            for t in 0..2 {
                let sl = &tile[t * 2 * TB..(t + 1) * 2 * TB];
                unsafe { tile_fma_many(sl, &a, 2, got.as_mut_ptr().add(t * 8), 16, 13) };
            }
            for (x, y) in got.iter().zip(&want) {
                assert!((x - y).abs() < 1e-4, "fma many {x} {y}");
            }
            if std::is_x86_feature_detected!("avxvnni") {
                got.fill(0.0);
                for t in 0..2 {
                    let sl = &tile[t * 2 * TB..(t + 1) * 2 * TB];
                    unsafe { tile_avxvnni_many(sl, &a, 2, got.as_mut_ptr().add(t * 8), 16, 13) };
                }
                for (x, y) in got.iter().zip(&want) {
                    assert!((x - y).abs() < 1e-4, "avxvnni many {x} {y}");
                }
            }
        }
    }
}
