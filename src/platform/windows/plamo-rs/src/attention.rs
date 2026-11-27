//! Specialized MQA-16 (head_dim = 128, F16 KV cache) FlashAttention for PLaMo 3 0.8B.
//!
//! Both decode and prefill load/convert each F16 K and V row once across all 16 query heads,
//! use AVX2+FMA+F16C microkernels with vectorized fast exp, and parallelize across the custom
//! spin-barrier thread pool. A scalar fallback preserves support for non-AVX2/F16C CPUs.
use super::pool::Pool;
use super::{H, LayerCache, Q};
use std::sync::atomic::{AtomicUsize, Ordering};

const HEADS: usize = Q / H;
const MAX_DECODE_CHUNKS: usize = 8;
const PARTIAL_STRIDE: usize = 2 + H; // M, S, VKQ[128]
const DECODE_BLOCK: usize = 64;
const PREFILL_Q_TILE: usize = 4;
const PREFILL_KV_TILE: usize = 32;

#[repr(C, align(32))]
struct Align32<T>(T);

/// Pure-Rust IEEE-754 binary32 -> binary16 (round-to-nearest-even).
#[inline]
pub(super) fn f32_to_f16_scalar(v: f32) -> u16 {
    let bits = v.to_bits();
    let sign = ((bits >> 16) & 0x8000) as u16;
    let exp = ((bits >> 23) & 0xFF) as i32;
    let frac = bits & 0x007F_FFFF;

    if exp == 0xFF {
        // Inf or NaN
        if frac == 0 {
            return sign | 0x7C00;
        }
        let payload = ((frac >> 13) as u16).max(1);
        return sign | 0x7C00 | payload;
    }

    let unbiased = exp - 127;
    if unbiased > 15 {
        return sign | 0x7C00; // overflow -> Inf
    }
    if unbiased < -24 {
        return sign; // underflow -> signed zero
    }
    if unbiased < -14 {
        // Subnormal binary16
        let mant = frac | 0x0080_0000;
        let shift = (-14 - unbiased) as u32 + 13;
        let half = 1u32 << (shift - 1);
        let mask = (1u32 << shift) - 1;
        let rem = mant & mask;
        let mut q = mant >> shift;
        if rem > half || (rem == half && (q & 1) != 0) {
            q += 1;
        }
        return sign | (q as u16);
    }

    // Normal binary16
    let h_exp = ((unbiased + 15) as u32) << 10;
    let round_bit = 1u32 << 12;
    let rem = frac & 0x1FFF;
    let mut h_frac = frac >> 13;
    if rem > round_bit || (rem == round_bit && (h_frac & 1) != 0) {
        h_frac += 1;
    }
    sign | ((h_exp + h_frac) as u16)
}

/// Pure-Rust IEEE-754 binary16 -> binary32.
#[inline]
pub(super) fn f16_to_f32_scalar(h: u16) -> f32 {
    let sign = ((h as u32) & 0x8000) << 16;
    let exp = ((h >> 10) & 0x1F) as u32;
    let frac = (h & 0x03FF) as u32;
    let bits = if exp == 0 {
        if frac == 0 {
            sign
        } else {
            let mut m = frac;
            let mut e = 127i32 - 14;
            while (m & 0x0400) == 0 {
                m <<= 1;
                e -= 1;
            }
            m &= 0x03FF;
            sign | ((e as u32) << 23) | (m << 13)
        }
    } else if exp == 31 {
        sign | 0x7F80_0000 | (frac << 13)
    } else {
        sign | ((exp + 112) << 23) | (frac << 13)
    };
    f32::from_bits(bits)
}

#[inline]
pub(super) fn f32_slice_to_f16(src: &[f32], dst: &mut [u16]) {
    debug_assert_eq!(src.len(), dst.len());
    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2")
        && std::is_x86_feature_detected!("f16c")
        && src.len() % 8 == 0
    {
        unsafe { f32_slice_to_f16_f16c(src, dst) };
        return;
    }
    for (d, &s) in dst.iter_mut().zip(src) {
        *d = f32_to_f16_scalar(s);
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,f16c")]
unsafe fn f32_slice_to_f16_f16c(src: &[f32], dst: &mut [u16]) {
    use std::arch::x86_64::*;
    let n = src.len();
    let mut i = 0;
    while i < n {
        unsafe {
            let v = _mm256_loadu_ps(src.as_ptr().add(i));
            let h = _mm256_cvtps_ph::<_MM_FROUND_TO_NEAREST_INT>(v);
            _mm_storeu_si128(dst.as_mut_ptr().add(i).cast(), h);
        }
        i += 8;
    }
}

/// An early query in a batch can still see rows evicted by later appends.
pub(super) struct EvictedRows {
    oldest: usize,
    retained_from: usize,
    keys: Vec<u16>,
    values: Vec<u16>,
}

impl EvictedRows {
    pub(super) fn capture(cache: &LayerCache, positions: &[usize]) -> Self {
        let oldest = (positions[0] + 1).saturating_sub(cache.capacity);
        let retained_from = (positions[positions.len() - 1] + 1).saturating_sub(cache.capacity);
        let len = retained_from.saturating_sub(oldest);
        let mut keys = Vec::with_capacity(len * H);
        let mut values = Vec::with_capacity(len * H);
        for pos in oldest..retained_from {
            let start = (pos % cache.capacity) * H;
            keys.extend_from_slice(&cache.k[start..start + H]);
            values.extend_from_slice(&cache.v[start..start + H]);
        }
        Self {
            oldest,
            retained_from,
            keys,
            values,
        }
    }
}

#[derive(Clone, Copy)]
struct DecodeWindow<'a> {
    k_first: &'a [u16],
    k_wrap: &'a [u16],
    v_first: &'a [u16],
    v_wrap: &'a [u16],
    first_rows: usize,
    len: usize,
}

impl<'a> DecodeWindow<'a> {
    #[inline]
    fn at(cache: &'a LayerCache, pos: usize) -> Self {
        let len = (pos + 1).min(cache.capacity);
        let oldest = pos + 1 - len;
        let start = oldest % cache.capacity;
        let first_rows = len.min(cache.capacity - start);
        let wrap_rows = len - first_rows;
        Self {
            k_first: &cache.k[start * H..(start + first_rows) * H],
            k_wrap: &cache.k[..wrap_rows * H],
            v_first: &cache.v[start * H..(start + first_rows) * H],
            v_wrap: &cache.v[..wrap_rows * H],
            first_rows,
            len,
        }
    }

    #[inline(always)]
    unsafe fn k_ptr(&self, idx: usize) -> *const u16 {
        if idx < self.first_rows {
            unsafe { self.k_first.as_ptr().add(idx * H) }
        } else {
            unsafe { self.k_wrap.as_ptr().add((idx - self.first_rows) * H) }
        }
    }

    #[inline(always)]
    unsafe fn v_ptr(&self, idx: usize) -> *const u16 {
        if idx < self.first_rows {
            unsafe { self.v_first.as_ptr().add(idx * H) }
        } else {
            unsafe { self.v_wrap.as_ptr().add((idx - self.first_rows) * H) }
        }
    }

    #[inline(always)]
    fn k_row(&self, idx: usize) -> &'a [u16] {
        if idx < self.first_rows {
            &self.k_first[idx * H..(idx + 1) * H]
        } else {
            let r = idx - self.first_rows;
            &self.k_wrap[r * H..(r + 1) * H]
        }
    }

    #[inline(always)]
    fn v_row(&self, idx: usize) -> &'a [u16] {
        if idx < self.first_rows {
            &self.v_first[idx * H..(idx + 1) * H]
        } else {
            let r = idx - self.first_rows;
            &self.v_wrap[r * H..(r + 1) * H]
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
#[inline]
unsafe fn fast_exp_avx2(mut x: std::arch::x86_64::__m256) -> std::arch::x86_64::__m256 {
    use std::arch::x86_64::*;
    let min_x = _mm256_set1_ps(-87.33654475f32);
    let valid = _mm256_cmp_ps::<_CMP_GT_OQ>(x, min_x);
    x = _mm256_max_ps(x, min_x);

    let log2e = _mm256_set1_ps(1.44269504088896341f32);
    let ln2_hi = _mm256_set1_ps(0.693359375f32);
    let ln2_lo = _mm256_set1_ps(-2.12194440e-4f32);

    let z = _mm256_mul_ps(x, log2e);
    let n = _mm256_round_ps::<{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }>(z);
    let f = _mm256_fnmadd_ps(n, ln2_hi, x);
    let f = _mm256_fnmadd_ps(n, ln2_lo, f);

    let c6 = _mm256_set1_ps(1.38888889e-3f32);
    let c5 = _mm256_set1_ps(8.33333333e-3f32);
    let c4 = _mm256_set1_ps(4.16666667e-2f32);
    let c3 = _mm256_set1_ps(1.66666667e-1f32);
    let c2 = _mm256_set1_ps(5.00000000e-1f32);
    let c1 = _mm256_set1_ps(1.0f32);

    let mut p = _mm256_fmadd_ps(c6, f, c5);
    p = _mm256_fmadd_ps(p, f, c4);
    p = _mm256_fmadd_ps(p, f, c3);
    p = _mm256_fmadd_ps(p, f, c2);
    p = _mm256_fmadd_ps(p, f, c1);
    p = _mm256_fmadd_ps(p, f, c1);

    let ni = _mm256_add_epi32(_mm256_cvtps_epi32(n), _mm256_set1_epi32(127));
    let pow2n = _mm256_castsi256_ps(_mm256_slli_epi32::<23>(ni));
    _mm256_and_ps(_mm256_mul_ps(p, pow2n), valid)
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
#[inline]
unsafe fn hsum256_ps(v: std::arch::x86_64::__m256) -> f32 {
    use std::arch::x86_64::*;
    let lo = _mm256_castps256_ps128(v);
    let hi = _mm256_extractf128_ps::<1>(v);
    let s = _mm_add_ps(lo, hi);
    let s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    let s = _mm_add_ss(s, _mm_shuffle_ps::<1>(s, s));
    _mm_cvtss_f32(s)
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
#[inline]
unsafe fn hmax256_ps(v: std::arch::x86_64::__m256) -> f32 {
    use std::arch::x86_64::*;
    let lo = _mm256_castps256_ps128(v);
    let hi = _mm256_extractf128_ps::<1>(v);
    let m = _mm_max_ps(lo, hi);
    let m = _mm_max_ps(m, _mm_movehl_ps(m, m));
    let m = _mm_max_ss(m, _mm_shuffle_ps::<1>(m, m));
    _mm_cvtss_f32(m)
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma,f16c")]
unsafe fn decode_chunk_avx2(
    window: DecodeWindow<'_>,
    qs: &[[f32; H]; HEADS],
    ic_start: usize,
    ic_end: usize,
    partials_out: Option<&mut [f32]>,
    direct_out: Option<&mut [f32]>,
) {
    use std::arch::x86_64::*;
    unsafe {
        let mut vkq = Align32([[0.0f32; H]; HEADS]);
        let mut m = Align32([f32::NEG_INFINITY; HEADS]);
        let mut s = Align32([0.0f32; HEADS]);
        let mut scores = Align32([[0.0f32; HEADS]; DECODE_BLOCK]);
        let mut kbuf = Align32([[0.0f32; H]; 2]);
        let mut vbuf = Align32([[0.0f32; H]; 8]);

        let mut ic = ic_start;
        while ic < ic_end {
            let cnt = (ic_end - ic).min(DECODE_BLOCK);

            // 1. QK^T for all 16 heads x cnt K rows (load & convert each K row ONCE).
            let mut t = 0;
            while t + 1 < cnt {
                let pk0 = window.k_ptr(ic + t);
                let pk1 = window.k_ptr(ic + t + 1);
                for d in (0..H).step_by(8) {
                    _mm256_store_ps(
                        kbuf.0[0].as_mut_ptr().add(d),
                        _mm256_cvtph_ps(_mm_loadu_si128(pk0.add(d).cast())),
                    );
                    _mm256_store_ps(
                        kbuf.0[1].as_mut_ptr().add(d),
                        _mm256_cvtph_ps(_mm_loadu_si128(pk1.add(d).cast())),
                    );
                }

                for h in (0..HEADS).step_by(4) {
                    let mut a00 = _mm256_setzero_ps();
                    let mut a01 = _mm256_setzero_ps();
                    let mut a10 = _mm256_setzero_ps();
                    let mut a11 = _mm256_setzero_ps();
                    let mut a20 = _mm256_setzero_ps();
                    let mut a21 = _mm256_setzero_ps();
                    let mut a30 = _mm256_setzero_ps();
                    let mut a31 = _mm256_setzero_ps();

                    let q0 = qs[h].as_ptr();
                    let q1 = qs[h + 1].as_ptr();
                    let q2 = qs[h + 2].as_ptr();
                    let q3 = qs[h + 3].as_ptr();
                    let k0p = kbuf.0[0].as_ptr();
                    let k1p = kbuf.0[1].as_ptr();

                    for d in (0..H).step_by(8) {
                        let k0 = _mm256_load_ps(k0p.add(d));
                        let k1 = _mm256_load_ps(k1p.add(d));
                        let vq0 = _mm256_load_ps(q0.add(d));
                        a00 = _mm256_fmadd_ps(vq0, k0, a00);
                        a01 = _mm256_fmadd_ps(vq0, k1, a01);
                        let vq1 = _mm256_load_ps(q1.add(d));
                        a10 = _mm256_fmadd_ps(vq1, k0, a10);
                        a11 = _mm256_fmadd_ps(vq1, k1, a11);
                        let vq2 = _mm256_load_ps(q2.add(d));
                        a20 = _mm256_fmadd_ps(vq2, k0, a20);
                        a21 = _mm256_fmadd_ps(vq2, k1, a21);
                        let vq3 = _mm256_load_ps(q3.add(d));
                        a30 = _mm256_fmadd_ps(vq3, k0, a30);
                        a31 = _mm256_fmadd_ps(vq3, k1, a31);
                    }

                    let s01_0 = _mm256_hadd_ps(a00, a10);
                    let s23_0 = _mm256_hadd_ps(a20, a30);
                    let s_0 = _mm256_hadd_ps(s01_0, s23_0);
                    let r_0 = _mm_add_ps(_mm256_castps256_ps128(s_0), _mm256_extractf128_ps::<1>(s_0));
                    _mm_store_ps(scores.0[t].as_mut_ptr().add(h), r_0);

                    let s01_1 = _mm256_hadd_ps(a01, a11);
                    let s23_1 = _mm256_hadd_ps(a21, a31);
                    let s_1 = _mm256_hadd_ps(s01_1, s23_1);
                    let r_1 = _mm_add_ps(_mm256_castps256_ps128(s_1), _mm256_extractf128_ps::<1>(s_1));
                    _mm_store_ps(scores.0[t + 1].as_mut_ptr().add(h), r_1);
                }
                t += 2;
            }
            if t < cnt {
                let pk0 = window.k_ptr(ic + t);
                for d in (0..H).step_by(8) {
                    _mm256_store_ps(
                        kbuf.0[0].as_mut_ptr().add(d),
                        _mm256_cvtph_ps(_mm_loadu_si128(pk0.add(d).cast())),
                    );
                }
                for h in (0..HEADS).step_by(4) {
                    let mut a0 = _mm256_setzero_ps();
                    let mut a1 = _mm256_setzero_ps();
                    let mut a2 = _mm256_setzero_ps();
                    let mut a3 = _mm256_setzero_ps();
                    let k0p = kbuf.0[0].as_ptr();
                    for d in (0..H).step_by(8) {
                        let k0 = _mm256_load_ps(k0p.add(d));
                        a0 = _mm256_fmadd_ps(_mm256_load_ps(qs[h].as_ptr().add(d)), k0, a0);
                        a1 = _mm256_fmadd_ps(_mm256_load_ps(qs[h + 1].as_ptr().add(d)), k0, a1);
                        a2 = _mm256_fmadd_ps(_mm256_load_ps(qs[h + 2].as_ptr().add(d)), k0, a2);
                        a3 = _mm256_fmadd_ps(_mm256_load_ps(qs[h + 3].as_ptr().add(d)), k0, a3);
                    }
                    let s01 = _mm256_hadd_ps(a0, a1);
                    let s23 = _mm256_hadd_ps(a2, a3);
                    let s_0 = _mm256_hadd_ps(s01, s23);
                    let r_0 = _mm_add_ps(_mm256_castps256_ps128(s_0), _mm256_extractf128_ps::<1>(s_0));
                    _mm_store_ps(scores.0[t].as_mut_ptr().add(h), r_0);
                }
            }

            // 2. Vectorized online softmax across all 16 heads.
            let m_old0 = _mm256_load_ps(m.0.as_ptr());
            let m_old1 = _mm256_load_ps(m.0.as_ptr().add(8));
            let mut m_new0 = m_old0;
            let mut m_new1 = m_old1;
            for i in 0..cnt {
                m_new0 = _mm256_max_ps(m_new0, _mm256_load_ps(scores.0[i].as_ptr()));
                m_new1 = _mm256_max_ps(m_new1, _mm256_load_ps(scores.0[i].as_ptr().add(8)));
            }

            let mut ms = Align32([0.0f32; HEADS]);
            let ms0 = fast_exp_avx2(_mm256_sub_ps(m_old0, m_new0));
            let ms1 = fast_exp_avx2(_mm256_sub_ps(m_old1, m_new1));
            _mm256_store_ps(ms.0.as_mut_ptr(), ms0);
            _mm256_store_ps(ms.0.as_mut_ptr().add(8), ms1);
            _mm256_store_ps(m.0.as_mut_ptr(), m_new0);
            _mm256_store_ps(m.0.as_mut_ptr().add(8), m_new1);

            let mut s_vec0 = _mm256_mul_ps(_mm256_load_ps(s.0.as_ptr()), ms0);
            let mut s_vec1 = _mm256_mul_ps(_mm256_load_ps(s.0.as_ptr().add(8)), ms1);

            for h in 0..HEADS {
                if ms.0[h] != 1.0 && s.0[h] != 0.0 {
                    let vms = _mm256_set1_ps(ms.0[h]);
                    let dst_v = vkq.0[h].as_mut_ptr();
                    for d in (0..H).step_by(8) {
                        _mm256_store_ps(dst_v.add(d), _mm256_mul_ps(_mm256_load_ps(dst_v.add(d)), vms));
                    }
                }
            }

            for i in 0..cnt {
                let p0 = fast_exp_avx2(_mm256_sub_ps(_mm256_load_ps(scores.0[i].as_ptr()), m_new0));
                let p1 = fast_exp_avx2(_mm256_sub_ps(_mm256_load_ps(scores.0[i].as_ptr().add(8)), m_new1));
                _mm256_store_ps(scores.0[i].as_mut_ptr(), p0);
                _mm256_store_ps(scores.0[i].as_mut_ptr().add(8), p1);
                s_vec0 = _mm256_add_ps(s_vec0, p0);
                s_vec1 = _mm256_add_ps(s_vec1, p1);
            }
            _mm256_store_ps(s.0.as_mut_ptr(), s_vec0);
            _mm256_store_ps(s.0.as_mut_ptr().add(8), s_vec1);

            // 3. Accumulate VKQ += P^T @ V (load & convert each V row ONCE across all 16 heads).
            let mut iv = 0;
            while iv + 8 <= cnt {
                for k8 in 0..8 {
                    let pv = window.v_ptr(ic + iv + k8);
                    let dst = vbuf.0[k8].as_mut_ptr();
                    for d in (0..H).step_by(8) {
                        _mm256_store_ps(dst.add(d), _mm256_cvtph_ps(_mm_loadu_si128(pv.add(d).cast())));
                    }
                }
                for h in 0..HEADS {
                    let p0 = _mm256_set1_ps(scores.0[iv][h]);
                    let p1 = _mm256_set1_ps(scores.0[iv + 1][h]);
                    let p2 = _mm256_set1_ps(scores.0[iv + 2][h]);
                    let p3 = _mm256_set1_ps(scores.0[iv + 3][h]);
                    let p4 = _mm256_set1_ps(scores.0[iv + 4][h]);
                    let p5 = _mm256_set1_ps(scores.0[iv + 5][h]);
                    let p6 = _mm256_set1_ps(scores.0[iv + 6][h]);
                    let p7 = _mm256_set1_ps(scores.0[iv + 7][h]);
                    let acc = vkq.0[h].as_mut_ptr();
                    for d in (0..H).step_by(16) {
                        let mut a0 = _mm256_load_ps(acc.add(d));
                        let mut a1 = _mm256_load_ps(acc.add(d + 8));
                        let mut b0 = _mm256_mul_ps(p1, _mm256_load_ps(vbuf.0[1].as_ptr().add(d)));
                        let mut b1 = _mm256_mul_ps(p1, _mm256_load_ps(vbuf.0[1].as_ptr().add(d + 8)));
                        a0 = _mm256_fmadd_ps(p0, _mm256_load_ps(vbuf.0[0].as_ptr().add(d)), a0);
                        a1 = _mm256_fmadd_ps(p0, _mm256_load_ps(vbuf.0[0].as_ptr().add(d + 8)), a1);
                        a0 = _mm256_fmadd_ps(p2, _mm256_load_ps(vbuf.0[2].as_ptr().add(d)), a0);
                        a1 = _mm256_fmadd_ps(p2, _mm256_load_ps(vbuf.0[2].as_ptr().add(d + 8)), a1);
                        b0 = _mm256_fmadd_ps(p3, _mm256_load_ps(vbuf.0[3].as_ptr().add(d)), b0);
                        b1 = _mm256_fmadd_ps(p3, _mm256_load_ps(vbuf.0[3].as_ptr().add(d + 8)), b1);
                        a0 = _mm256_fmadd_ps(p4, _mm256_load_ps(vbuf.0[4].as_ptr().add(d)), a0);
                        a1 = _mm256_fmadd_ps(p4, _mm256_load_ps(vbuf.0[4].as_ptr().add(d + 8)), a1);
                        b0 = _mm256_fmadd_ps(p5, _mm256_load_ps(vbuf.0[5].as_ptr().add(d)), b0);
                        b1 = _mm256_fmadd_ps(p5, _mm256_load_ps(vbuf.0[5].as_ptr().add(d + 8)), b1);
                        a0 = _mm256_fmadd_ps(p6, _mm256_load_ps(vbuf.0[6].as_ptr().add(d)), a0);
                        a1 = _mm256_fmadd_ps(p6, _mm256_load_ps(vbuf.0[6].as_ptr().add(d + 8)), a1);
                        b0 = _mm256_fmadd_ps(p7, _mm256_load_ps(vbuf.0[7].as_ptr().add(d)), b0);
                        b1 = _mm256_fmadd_ps(p7, _mm256_load_ps(vbuf.0[7].as_ptr().add(d + 8)), b1);
                        _mm256_store_ps(acc.add(d), _mm256_add_ps(a0, b0));
                        _mm256_store_ps(acc.add(d + 8), _mm256_add_ps(a1, b1));
                    }
                }
                iv += 8;
            }
            let rem = cnt - iv;
            if rem > 0 {
                for k8 in 0..rem {
                    let pv = window.v_ptr(ic + iv + k8);
                    let dst = vbuf.0[k8].as_mut_ptr();
                    for d in (0..H).step_by(8) {
                        _mm256_store_ps(dst.add(d), _mm256_cvtph_ps(_mm_loadu_si128(pv.add(d).cast())));
                    }
                }
                for h in 0..HEADS {
                    let acc = vkq.0[h].as_mut_ptr();
                    for k8 in 0..rem {
                        let pk = _mm256_set1_ps(scores.0[iv + k8][h]);
                        let vp = vbuf.0[k8].as_ptr();
                        for d in (0..H).step_by(8) {
                            let v_acc = _mm256_load_ps(acc.add(d));
                            _mm256_store_ps(acc.add(d), _mm256_fmadd_ps(pk, _mm256_load_ps(vp.add(d)), v_acc));
                        }
                    }
                }
            }

            ic += cnt;
        }

        if let Some(partials) = partials_out {
            // Layout: chunk-major partials[h * PARTIAL_STRIDE ..]
            for h in 0..HEADS {
                let dst = &mut partials[h * PARTIAL_STRIDE..(h + 1) * PARTIAL_STRIDE];
                dst[0] = m.0[h];
                dst[1] = s.0[h];
                dst[2..].copy_from_slice(&vkq.0[h]);
            }
        } else if let Some(out) = direct_out {
            for h in 0..HEADS {
                let inv = if s.0[h] == 0.0 { 0.0 } else { 1.0 / s.0[h] };
                let vinv = _mm256_set1_ps(inv);
                let out_ptr = out.as_mut_ptr().add(h * H);
                let src_ptr = vkq.0[h].as_ptr();
                for d in (0..H).step_by(8) {
                    _mm256_storeu_ps(out_ptr.add(d), _mm256_mul_ps(_mm256_load_ps(src_ptr.add(d)), vinv));
                }
            }
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
unsafe fn reduce_decode_partials_avx2(partials: &[f32], n_chunks: usize, out: &mut [f32]) {
    use std::arch::x86_64::*;
    let chunk_stride = HEADS * PARTIAL_STRIDE;
    unsafe {
        for h in 0..HEADS {
            let mut m_final = f32::NEG_INFINITY;
            let mut s_final = 0.0f32;
            let mut vkq = Align32([0.0f32; H]);
            for c in 0..n_chunks {
                let p = &partials[c * chunk_stride + h * PARTIAL_STRIDE..];
                let m_c = p[0];
                let s_c = p[1];
                if s_c == 0.0 {
                    continue;
                }
                if s_final == 0.0 {
                    m_final = m_c;
                    s_final = s_c;
                    vkq.0.copy_from_slice(&p[2..2 + H]);
                } else {
                    let m_new = m_final.max(m_c);
                    let scale_old = (m_final - m_new).exp();
                    let scale_new = (m_c - m_new).exp();
                    let v_so = _mm256_set1_ps(scale_old);
                    let v_sn = _mm256_set1_ps(scale_new);
                    let vp = vkq.0.as_mut_ptr();
                    let cp = p.as_ptr().add(2);
                    for d in (0..H).step_by(8) {
                        let vf = _mm256_mul_ps(_mm256_load_ps(vp.add(d)), v_so);
                        let vf = _mm256_fmadd_ps(_mm256_loadu_ps(cp.add(d)), v_sn, vf);
                        _mm256_store_ps(vp.add(d), vf);
                    }
                    s_final = s_final * scale_old + s_c * scale_new;
                    m_final = m_new;
                }
            }
            let inv = if s_final == 0.0 { 0.0 } else { 1.0 / s_final };
            let vinv = _mm256_set1_ps(inv);
            let out_ptr = out.as_mut_ptr().add(h * H);
            let vp = vkq.0.as_ptr();
            for d in (0..H).step_by(8) {
                _mm256_storeu_ps(out_ptr.add(d), _mm256_mul_ps(_mm256_load_ps(vp.add(d)), vinv));
            }
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma,f16c")]
unsafe fn single_avx2(
    pool: &Pool,
    query: &[f32],
    window: DecodeWindow<'_>,
    out: &mut [f32],
    scratch: &mut Vec<f32>,
) {
    use std::arch::x86_64::*;
    let scale = 1.0f32 / (H as f32).sqrt();
    let mut qs = Align32([[0.0f32; H]; HEADS]);
    unsafe {
        let vscale = _mm256_set1_ps(scale);
        for h in 0..HEADS {
            let src = query.as_ptr().add(h * H);
            let dst = qs.0[h].as_mut_ptr();
            for d in (0..H).step_by(8) {
                _mm256_store_ps(dst.add(d), _mm256_mul_ps(_mm256_loadu_ps(src.add(d)), vscale));
            }
        }
    }

    let nth = pool.decode_threads();
    if window.len <= 128 || nth <= 1 {
        unsafe { decode_chunk_avx2(window, &qs.0, 0, window.len, None, Some(out)) };
        return;
    }

    let max_chunks = nth.min(MAX_DECODE_CHUNKS);
    let n_chunks = max_chunks.min((window.len + 63) / 64).max(1);
    if n_chunks <= 1 {
        unsafe { decode_chunk_avx2(window, &qs.0, 0, window.len, None, Some(out)) };
        return;
    }

    let chunk_size = (window.len + n_chunks - 1) / n_chunks;
    let chunk_stride = HEADS * PARTIAL_STRIDE;
    let total_partials = n_chunks * chunk_stride;
    if scratch.len() < total_partials {
        scratch.resize(total_partials, 0.0);
    }

    let partials_ptr = scratch.as_mut_ptr() as usize;
    let qs_ref = &qs.0;
    pool.dispatch(n_chunks, |ith, _| {
        let ic_start = ith * chunk_size;
        let ic_end = (ic_start + chunk_size).min(window.len);
        let dst = unsafe {
            std::slice::from_raw_parts_mut(
                (partials_ptr as *mut f32).add(ith * chunk_stride),
                chunk_stride,
            )
        };
        if ic_start < ic_end {
            unsafe { decode_chunk_avx2(window, qs_ref, ic_start, ic_end, Some(dst), None) };
        } else {
            for h in 0..HEADS {
                dst[h * PARTIAL_STRIDE] = f32::NEG_INFINITY;
                dst[h * PARTIAL_STRIDE + 1] = 0.0;
            }
        }
    });

    unsafe { reduce_decode_partials_avx2(&scratch[..total_partials], n_chunks, out) };
}

fn single_scalar(query: &[f32], window: DecodeWindow<'_>, out: &mut [f32], scratch: &mut Vec<f32>) {
    let scale = 1.0f32 / (H as f32).sqrt();
    if scratch.len() < window.len {
        scratch.resize(window.len, 0.0);
    }
    let scores = &mut scratch[..window.len];
    for (h, (q, out_h)) in query
        .chunks_exact(H)
        .zip(out.chunks_exact_mut(H))
        .enumerate()
    {
        let _ = h;
        let mut max = f32::NEG_INFINITY;
        for (idx, s) in scores.iter_mut().enumerate() {
            let k_row = window.k_row(idx);
            let mut dot = 0.0f32;
            for d in 0..H {
                dot += q[d] * f16_to_f32_scalar(k_row[d]);
            }
            *s = dot * scale;
            max = max.max(*s);
        }
        let mut sum = 0.0f32;
        for s in scores.iter_mut() {
            *s = (*s - max).exp();
            sum += *s;
        }
        let inv = if sum == 0.0 { 0.0 } else { 1.0 / sum };
        out_h.fill(0.0);
        for (idx, &p) in scores.iter().enumerate() {
            let w = p * inv;
            let v_row = window.v_row(idx);
            for d in 0..H {
                out_h[d] += w * f16_to_f32_scalar(v_row[d]);
            }
        }
    }
}

pub(super) fn single(
    pool: &Pool,
    query: &[f32],
    cache: &LayerCache,
    pos: usize,
    out: &mut [f32],
    scratch: &mut Vec<f32>,
) {
    let window = DecodeWindow::at(cache, pos);
    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2")
        && std::is_x86_feature_detected!("fma")
        && std::is_x86_feature_detected!("f16c")
    {
        unsafe { single_avx2(pool, query, window, out, scratch) };
        return;
    }
    let _ = pool;
    single_scalar(query, window, out, scratch);
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
#[inline]
unsafe fn transpose_8x8_ps(
    r0: &mut std::arch::x86_64::__m256,
    r1: &mut std::arch::x86_64::__m256,
    r2: &mut std::arch::x86_64::__m256,
    r3: &mut std::arch::x86_64::__m256,
    r4: &mut std::arch::x86_64::__m256,
    r5: &mut std::arch::x86_64::__m256,
    r6: &mut std::arch::x86_64::__m256,
    r7: &mut std::arch::x86_64::__m256,
) {
    use std::arch::x86_64::*;
    let t0 = _mm256_unpacklo_ps(*r0, *r1);
    let t1 = _mm256_unpackhi_ps(*r0, *r1);
    let t2 = _mm256_unpacklo_ps(*r2, *r3);
    let t3 = _mm256_unpackhi_ps(*r2, *r3);
    let t4 = _mm256_unpacklo_ps(*r4, *r5);
    let t5 = _mm256_unpackhi_ps(*r4, *r5);
    let t6 = _mm256_unpacklo_ps(*r6, *r7);
    let t7 = _mm256_unpackhi_ps(*r6, *r7);

    let s0 = _mm256_shuffle_ps::<0x44>(t0, t2);
    let s1 = _mm256_shuffle_ps::<0xEE>(t0, t2);
    let s2 = _mm256_shuffle_ps::<0x44>(t1, t3);
    let s3 = _mm256_shuffle_ps::<0xEE>(t1, t3);
    let s4 = _mm256_shuffle_ps::<0x44>(t4, t6);
    let s5 = _mm256_shuffle_ps::<0xEE>(t4, t6);
    let s6 = _mm256_shuffle_ps::<0x44>(t5, t7);
    let s7 = _mm256_shuffle_ps::<0xEE>(t5, t7);

    *r0 = _mm256_permute2f128_ps::<0x20>(s0, s4);
    *r1 = _mm256_permute2f128_ps::<0x20>(s1, s5);
    *r2 = _mm256_permute2f128_ps::<0x20>(s2, s6);
    *r3 = _mm256_permute2f128_ps::<0x20>(s3, s7);
    *r4 = _mm256_permute2f128_ps::<0x31>(s0, s4);
    *r5 = _mm256_permute2f128_ps::<0x31>(s1, s5);
    *r6 = _mm256_permute2f128_ps::<0x31>(s2, s6);
    *r7 = _mm256_permute2f128_ps::<0x31>(s3, s7);
}

/// Computes `out[4][32] = q[4][128] @ k_t[128][32]`.
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
#[inline]
unsafe fn gemm_4x32_k128(q: &[f32; PREFILL_Q_TILE * H], k_t: &[f32; H * PREFILL_KV_TILE], out: &mut [f32; PREFILL_Q_TILE * PREFILL_KV_TILE]) {
    use std::arch::x86_64::*;
    unsafe {
        let q0 = q.as_ptr();
        let q1 = q.as_ptr().add(H);
        let q2 = q.as_ptr().add(2 * H);
        let q3 = q.as_ptr().add(3 * H);
        for half in 0..2 {
            let c0 = half * 16;
            let mut a00 = _mm256_setzero_ps();
            let mut a01 = _mm256_setzero_ps();
            let mut a10 = _mm256_setzero_ps();
            let mut a11 = _mm256_setzero_ps();
            let mut a20 = _mm256_setzero_ps();
            let mut a21 = _mm256_setzero_ps();
            let mut a30 = _mm256_setzero_ps();
            let mut a31 = _mm256_setzero_ps();

            let mut kp = k_t.as_ptr().add(c0);
            for d in 0..H {
                let k0 = _mm256_load_ps(kp);
                let k1 = _mm256_load_ps(kp.add(8));
                kp = kp.add(PREFILL_KV_TILE);

                let vq0 = _mm256_broadcast_ss(&*q0.add(d));
                a00 = _mm256_fmadd_ps(vq0, k0, a00);
                a01 = _mm256_fmadd_ps(vq0, k1, a01);

                let vq1 = _mm256_broadcast_ss(&*q1.add(d));
                a10 = _mm256_fmadd_ps(vq1, k0, a10);
                a11 = _mm256_fmadd_ps(vq1, k1, a11);

                let vq2 = _mm256_broadcast_ss(&*q2.add(d));
                a20 = _mm256_fmadd_ps(vq2, k0, a20);
                a21 = _mm256_fmadd_ps(vq2, k1, a21);

                let vq3 = _mm256_broadcast_ss(&*q3.add(d));
                a30 = _mm256_fmadd_ps(vq3, k0, a30);
                a31 = _mm256_fmadd_ps(vq3, k1, a31);
            }

            let op = out.as_mut_ptr().add(c0);
            _mm256_store_ps(op, a00);
            _mm256_store_ps(op.add(8), a01);
            _mm256_store_ps(op.add(PREFILL_KV_TILE), a10);
            _mm256_store_ps(op.add(PREFILL_KV_TILE + 8), a11);
            _mm256_store_ps(op.add(2 * PREFILL_KV_TILE), a20);
            _mm256_store_ps(op.add(2 * PREFILL_KV_TILE + 8), a21);
            _mm256_store_ps(op.add(3 * PREFILL_KV_TILE), a30);
            _mm256_store_ps(op.add(3 * PREFILL_KV_TILE + 8), a31);
        }
    }
}

/// Accumulates `acc[4][128] += p[4][kv_tile] @ v[kv_tile][128]`.
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
#[inline]
unsafe fn gemm_4x128_k32(
    p: &[f32; PREFILL_Q_TILE * PREFILL_KV_TILE],
    v: &[f32; PREFILL_KV_TILE * H],
    acc: &mut [f32; PREFILL_Q_TILE * H],
    kv_tile: usize,
) {
    use std::arch::x86_64::*;
    unsafe {
        let p0 = p.as_ptr();
        let p1 = p.as_ptr().add(PREFILL_KV_TILE);
        let p2 = p.as_ptr().add(2 * PREFILL_KV_TILE);
        let p3 = p.as_ptr().add(3 * PREFILL_KV_TILE);

        for d in (0..H).step_by(16) {
            let ap = acc.as_mut_ptr().add(d);
            let mut a00 = _mm256_load_ps(ap);
            let mut a01 = _mm256_load_ps(ap.add(8));
            let mut a10 = _mm256_load_ps(ap.add(H));
            let mut a11 = _mm256_load_ps(ap.add(H + 8));
            let mut a20 = _mm256_load_ps(ap.add(2 * H));
            let mut a21 = _mm256_load_ps(ap.add(2 * H + 8));
            let mut a30 = _mm256_load_ps(ap.add(3 * H));
            let mut a31 = _mm256_load_ps(ap.add(3 * H + 8));

            let mut vp = v.as_ptr().add(d);
            for tk in 0..kv_tile {
                let v0 = _mm256_load_ps(vp);
                let v1 = _mm256_load_ps(vp.add(8));
                vp = vp.add(H);

                let w0 = _mm256_broadcast_ss(&*p0.add(tk));
                a00 = _mm256_fmadd_ps(w0, v0, a00);
                a01 = _mm256_fmadd_ps(w0, v1, a01);

                let w1 = _mm256_broadcast_ss(&*p1.add(tk));
                a10 = _mm256_fmadd_ps(w1, v0, a10);
                a11 = _mm256_fmadd_ps(w1, v1, a11);

                let w2 = _mm256_broadcast_ss(&*p2.add(tk));
                a20 = _mm256_fmadd_ps(w2, v0, a20);
                a21 = _mm256_fmadd_ps(w2, v1, a21);

                let w3 = _mm256_broadcast_ss(&*p3.add(tk));
                a30 = _mm256_fmadd_ps(w3, v0, a30);
                a31 = _mm256_fmadd_ps(w3, v1, a31);
            }

            _mm256_store_ps(ap, a00);
            _mm256_store_ps(ap.add(8), a01);
            _mm256_store_ps(ap.add(H), a10);
            _mm256_store_ps(ap.add(H + 8), a11);
            _mm256_store_ps(ap.add(2 * H), a20);
            _mm256_store_ps(ap.add(2 * H + 8), a21);
            _mm256_store_ps(ap.add(3 * H), a30);
            _mm256_store_ps(ap.add(3 * H + 8), a31);
        }
    }
}

#[inline(always)]
fn kv_pos_ptrs(cache: &LayerCache, evicted: &EvictedRows, pos: usize) -> (*const u16, *const u16) {
    if pos < evicted.retained_from {
        let off = (pos - evicted.oldest) * H;
        unsafe {
            (
                evicted.keys.as_ptr().add(off),
                evicted.values.as_ptr().add(off),
            )
        }
    } else {
        let off = (pos % cache.capacity) * H;
        unsafe { (cache.k.as_ptr().add(off), cache.v.as_ptr().add(off)) }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma,f16c")]
unsafe fn prefill_qtile_avx2(
    qkv: &[f32],
    cache: &LayerCache,
    positions: &[usize],
    evicted: &EvictedRows,
    out_ptr: *mut f32,
    iq0: usize,
) {
    use std::arch::x86_64::*;
    let tile_rows = PREFILL_Q_TILE.min(positions.len() - iq0);
    let scale = 1.0f32 / (H as f32).sqrt();
    let row_stride = Q + 2 * H;

    let mut q_all = Align32([[0.0f32; PREFILL_Q_TILE * H]; HEADS]);
    let mut vkq_all = Align32([[0.0f32; PREFILL_Q_TILE * H]; HEADS]);
    let mut m_all = [[f32::NEG_INFINITY; PREFILL_Q_TILE]; HEADS];
    let mut s_all = [[0.0f32; PREFILL_Q_TILE]; HEADS];
    let mut k_f32 = Align32([0.0f32; H * PREFILL_KV_TILE]);
    let mut v_f32 = Align32([0.0f32; PREFILL_KV_TILE * H]);
    let mut kq = Align32([0.0f32; PREFILL_Q_TILE * PREFILL_KV_TILE]);

    let mut q_pos = [0usize; PREFILL_Q_TILE];
    let mut q_oldest = [0usize; PREFILL_Q_TILE];
    for tq in 0..tile_rows {
        let p = positions[iq0 + tq];
        q_pos[tq] = p;
        q_oldest[tq] = (p + 1).saturating_sub(cache.capacity);
    }

    let tile_kv_begin = q_oldest[0];
    let tile_kv_end = q_pos[tile_rows - 1] + 1;
    let first_q_pos = q_pos[0];
    let last_q_oldest = q_oldest[tile_rows - 1];

    unsafe {
        let vscale = _mm256_set1_ps(scale);
        for h in 0..HEADS {
            for tq in 0..tile_rows {
                let pq = qkv.as_ptr().add((iq0 + tq) * row_stride + h * H);
                let dst = q_all.0[h].as_mut_ptr().add(tq * H);
                for d in (0..H).step_by(8) {
                    _mm256_store_ps(dst.add(d), _mm256_mul_ps(_mm256_loadu_ps(pq.add(d)), vscale));
                }
            }
        }

        let mut k_start = tile_kv_begin;
        while k_start < tile_kv_end {
            let kv_tile = PREFILL_KV_TILE.min(tile_kv_end - k_start);

            // Convert & 8x8-transpose K tile ONCE for all 16 heads.
            if kv_tile < PREFILL_KV_TILE {
                k_f32.0.fill(0.0);
            }
            let mut tk0 = 0;
            while tk0 < kv_tile {
                let ntk = 8.min(kv_tile - tk0);
                if ntk == 8 {
                    let pk: [*const u16; 8] =
                        std::array::from_fn(|i| kv_pos_ptrs(cache, evicted, k_start + tk0 + i).0);
                    for d in (0..H).step_by(8) {
                        let mut r0 = _mm256_cvtph_ps(_mm_loadu_si128(pk[0].add(d).cast()));
                        let mut r1 = _mm256_cvtph_ps(_mm_loadu_si128(pk[1].add(d).cast()));
                        let mut r2 = _mm256_cvtph_ps(_mm_loadu_si128(pk[2].add(d).cast()));
                        let mut r3 = _mm256_cvtph_ps(_mm_loadu_si128(pk[3].add(d).cast()));
                        let mut r4 = _mm256_cvtph_ps(_mm_loadu_si128(pk[4].add(d).cast()));
                        let mut r5 = _mm256_cvtph_ps(_mm_loadu_si128(pk[5].add(d).cast()));
                        let mut r6 = _mm256_cvtph_ps(_mm_loadu_si128(pk[6].add(d).cast()));
                        let mut r7 = _mm256_cvtph_ps(_mm_loadu_si128(pk[7].add(d).cast()));
                        transpose_8x8_ps(
                            &mut r0, &mut r1, &mut r2, &mut r3, &mut r4, &mut r5, &mut r6, &mut r7,
                        );
                        let kp = k_f32.0.as_mut_ptr();
                        _mm256_store_ps(kp.add((d + 0) * PREFILL_KV_TILE + tk0), r0);
                        _mm256_store_ps(kp.add((d + 1) * PREFILL_KV_TILE + tk0), r1);
                        _mm256_store_ps(kp.add((d + 2) * PREFILL_KV_TILE + tk0), r2);
                        _mm256_store_ps(kp.add((d + 3) * PREFILL_KV_TILE + tk0), r3);
                        _mm256_store_ps(kp.add((d + 4) * PREFILL_KV_TILE + tk0), r4);
                        _mm256_store_ps(kp.add((d + 5) * PREFILL_KV_TILE + tk0), r5);
                        _mm256_store_ps(kp.add((d + 6) * PREFILL_KV_TILE + tk0), r6);
                        _mm256_store_ps(kp.add((d + 7) * PREFILL_KV_TILE + tk0), r7);
                    }
                } else {
                    for i in 0..ntk {
                        let pk = kv_pos_ptrs(cache, evicted, k_start + tk0 + i).0;
                        let mut tmp = [0.0f32; H];
                        for d in (0..H).step_by(8) {
                            _mm256_storeu_ps(
                                tmp.as_mut_ptr().add(d),
                                _mm256_cvtph_ps(_mm_loadu_si128(pk.add(d).cast())),
                            );
                        }
                        for d in 0..H {
                            k_f32.0[d * PREFILL_KV_TILE + tk0 + i] = tmp[d];
                        }
                    }
                }
                tk0 += 8;
            }

            // Convert V tile ONCE for all 16 heads.
            for tk in 0..kv_tile {
                let pv = kv_pos_ptrs(cache, evicted, k_start + tk).1;
                let dst = v_f32.0.as_mut_ptr().add(tk * H);
                for d in (0..H).step_by(8) {
                    _mm256_store_ps(dst.add(d), _mm256_cvtph_ps(_mm_loadu_si128(pv.add(d).cast())));
                }
            }

            let all_unmasked =
                kv_tile == PREFILL_KV_TILE && k_start >= last_q_oldest && k_start + kv_tile - 1 <= first_q_pos;

            for h in 0..HEADS {
                gemm_4x32_k128(&q_all.0[h], &k_f32.0, &mut kq.0);

                if !all_unmasked {
                    for tq in 0..tile_rows {
                        let row = &mut kq.0[tq * PREFILL_KV_TILE..(tq + 1) * PREFILL_KV_TILE];
                        let q_lo = q_oldest[tq];
                        let q_hi = q_pos[tq];
                        for tk in 0..kv_tile {
                            let kp = k_start + tk;
                            if kp < q_lo || kp > q_hi {
                                row[tk] = f32::NEG_INFINITY;
                            }
                        }
                        for tk in kv_tile..PREFILL_KV_TILE {
                            row[tk] = f32::NEG_INFINITY;
                        }
                    }
                }

                for tq in 0..tile_rows {
                    let row_ptr = kq.0.as_mut_ptr().add(tq * PREFILL_KV_TILE);
                    let vm0 = _mm256_max_ps(_mm256_load_ps(row_ptr), _mm256_load_ps(row_ptr.add(8)));
                    let vm1 = _mm256_max_ps(_mm256_load_ps(row_ptr.add(16)), _mm256_load_ps(row_ptr.add(24)));
                    let tile_max = hmax256_ps(_mm256_max_ps(vm0, vm1));

                    if tile_max == f32::NEG_INFINITY {
                        let z = _mm256_setzero_ps();
                        _mm256_store_ps(row_ptr, z);
                        _mm256_store_ps(row_ptr.add(8), z);
                        _mm256_store_ps(row_ptr.add(16), z);
                        _mm256_store_ps(row_ptr.add(24), z);
                        continue;
                    }

                    let m_old = m_all[h][tq];
                    let m_new = m_old.max(tile_max);
                    if m_new > m_old && s_all[h][tq] != 0.0 {
                        let ms = (m_old - m_new).exp();
                        let vms = _mm256_set1_ps(ms);
                        let dst_vkq = vkq_all.0[h].as_mut_ptr().add(tq * H);
                        for d in (0..H).step_by(8) {
                            _mm256_store_ps(
                                dst_vkq.add(d),
                                _mm256_mul_ps(_mm256_load_ps(dst_vkq.add(d)), vms),
                            );
                        }
                        s_all[h][tq] *= ms;
                    }
                    m_all[h][tq] = m_new;

                    let vmn = _mm256_set1_ps(m_new);
                    let p0 = fast_exp_avx2(_mm256_sub_ps(_mm256_load_ps(row_ptr), vmn));
                    let p1 = fast_exp_avx2(_mm256_sub_ps(_mm256_load_ps(row_ptr.add(8)), vmn));
                    let p2 = fast_exp_avx2(_mm256_sub_ps(_mm256_load_ps(row_ptr.add(16)), vmn));
                    let p3 = fast_exp_avx2(_mm256_sub_ps(_mm256_load_ps(row_ptr.add(24)), vmn));
                    _mm256_store_ps(row_ptr, p0);
                    _mm256_store_ps(row_ptr.add(8), p1);
                    _mm256_store_ps(row_ptr.add(16), p2);
                    _mm256_store_ps(row_ptr.add(24), p3);
                    let psum = _mm256_add_ps(_mm256_add_ps(p0, p1), _mm256_add_ps(p2, p3));
                    s_all[h][tq] += hsum256_ps(psum);
                }

                for tq in tile_rows..PREFILL_Q_TILE {
                    let row_ptr = kq.0.as_mut_ptr().add(tq * PREFILL_KV_TILE);
                    let z = _mm256_setzero_ps();
                    _mm256_store_ps(row_ptr, z);
                    _mm256_store_ps(row_ptr.add(8), z);
                    _mm256_store_ps(row_ptr.add(16), z);
                    _mm256_store_ps(row_ptr.add(24), z);
                }

                gemm_4x128_k32(&kq.0, &v_f32.0, &mut vkq_all.0[h], kv_tile);
            }

            k_start += kv_tile;
        }

        // Normalize and store all 16 heads x tile_rows into out[iq * Q + h * H].
        for h in 0..HEADS {
            for tq in 0..tile_rows {
                let inv = if s_all[h][tq] == 0.0 {
                    0.0
                } else {
                    1.0 / s_all[h][tq]
                };
                let vinv = _mm256_set1_ps(inv);
                let src = vkq_all.0[h].as_ptr().add(tq * H);
                let dst = out_ptr.add((iq0 + tq) * Q + h * H);
                for d in (0..H).step_by(8) {
                    _mm256_storeu_ps(dst.add(d), _mm256_mul_ps(_mm256_load_ps(src.add(d)), vinv));
                }
            }
        }
    }
}

fn prefill_scalar(
    qkv: &[f32],
    cache: &LayerCache,
    positions: &[usize],
    evicted: &EvictedRows,
    out: &mut [f32],
) {
    let scale = 1.0f32 / (H as f32).sqrt();
    let row_stride = Q + 2 * H;
    let mut scores = Vec::new();
    for (j, &pos) in positions.iter().enumerate() {
        let len = (pos + 1).min(cache.capacity);
        let oldest = pos + 1 - len;
        scores.resize(len, 0.0);
        let q_row = &qkv[j * row_stride..j * row_stride + Q];
        let o_row = &mut out[j * Q..(j + 1) * Q];
        for (q, oh) in q_row.chunks_exact(H).zip(o_row.chunks_exact_mut(H)) {
            let mut max = f32::NEG_INFINITY;
            for (idx, s) in scores.iter_mut().enumerate() {
                let pk = kv_pos_ptrs(cache, evicted, oldest + idx).0;
                let k_slice = unsafe { std::slice::from_raw_parts(pk, H) };
                let mut dot = 0.0f32;
                for d in 0..H {
                    dot += q[d] * f16_to_f32_scalar(k_slice[d]);
                }
                *s = dot * scale;
                max = max.max(*s);
            }
            let mut sum = 0.0f32;
            for s in scores.iter_mut() {
                *s = (*s - max).exp();
                sum += *s;
            }
            let inv = if sum == 0.0 { 0.0 } else { 1.0 / sum };
            oh.fill(0.0);
            for (idx, &p) in scores.iter().enumerate() {
                let w = p * inv;
                let pv = kv_pos_ptrs(cache, evicted, oldest + idx).1;
                let v_slice = unsafe { std::slice::from_raw_parts(pv, H) };
                for d in 0..H {
                    oh[d] += w * f16_to_f32_scalar(v_slice[d]);
                }
            }
        }
    }
}

pub(super) fn prefill(
    pool: &Pool,
    qkv: &[f32],
    cache: &LayerCache,
    positions: &[usize],
    evicted: &EvictedRows,
    out: &mut [f32],
) {
    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2")
        && std::is_x86_feature_detected!("fma")
        && std::is_x86_feature_detected!("f16c")
    {
        let n_qtiles = (positions.len() + PREFILL_Q_TILE - 1) / PREFILL_Q_TILE;
        let next_tile = AtomicUsize::new(0);
        let out_addr = out.as_mut_ptr() as usize;
        pool.dispatch(n_qtiles.min(pool.num_threads()), |_, _| {
            loop {
                let tile = next_tile.fetch_add(1, Ordering::Relaxed);
                if tile >= n_qtiles {
                    break;
                }
                unsafe {
                    prefill_qtile_avx2(
                        qkv,
                        cache,
                        positions,
                        evicted,
                        out_addr as *mut f32,
                        tile * PREFILL_Q_TILE,
                    );
                }
            }
        });
        return;
    }
    let _ = pool;
    prefill_scalar(qkv, cache, positions, evicted, out);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vector<const N: usize>(seed: usize) -> [f32; N] {
        std::array::from_fn(|i| ((i * 31 + seed * 17) % 127) as f32 / 127.0 - 0.5)
    }

    fn reference_f16(cache: &LayerCache, query: &[f32], pos: usize) -> Vec<f32> {
        let len = (pos + 1).min(cache.capacity);
        let scale = 1.0 / (H as f32).sqrt();
        let row = |idx| ((pos + 1 - len + idx) % cache.capacity) * H;
        let mut out = vec![0.0; Q];
        for (q, out) in query.chunks_exact(H).zip(out.chunks_exact_mut(H)) {
            let mut scores = vec![0.0; len];
            let mut max = f32::NEG_INFINITY;
            for (idx, s) in scores.iter_mut().enumerate() {
                let k = &cache.k[row(idx)..row(idx) + H];
                let dot: f32 = q
                    .iter()
                    .zip(k)
                    .map(|(a, &b)| a * f16_to_f32_scalar(b))
                    .sum();
                *s = dot * scale;
                max = max.max(*s);
            }
            let mut sum = 0.0;
            for s in &mut scores {
                *s = (*s - max).exp();
                sum += *s;
            }
            for (idx, s) in scores.into_iter().enumerate() {
                for (out, &v) in out.iter_mut().zip(&cache.v[row(idx)..row(idx) + H]) {
                    *out += (s / sum) * f16_to_f32_scalar(v);
                }
            }
        }
        out
    }

    #[test]
    fn f16_roundtrip_matches_f16c() {
        let vals: [f32; 16] = [
            0.0, -0.0, 1.0, -1.0, 0.5, -0.125, 3.14159, -42.25, 1e-5, -1e-6, 65504.0, -65504.0,
            1e-8, 0.3333333, 123.456, -0.00006103515625,
        ];
        let mut hw = [0u16; 16];
        f32_slice_to_f16(&vals, &mut hw);
        for (i, &v) in vals.iter().enumerate() {
            let sw = f32_to_f16_scalar(v);
            assert_eq!(hw[i], sw, "mismatch at {i}: val={v} hw={:04x} sw={:04x}", hw[i], sw);
            let back = f16_to_f32_scalar(sw);
            if v.abs() > 1e-4 {
                assert!((back - v).abs() <= v.abs() * 1e-3 + 1e-4);
            }
        }
    }

    #[test]
    fn parallel_heads_preserve_wrapped_window_order() {
        let pool = Pool::new(4).unwrap();
        let _guard = pool.enter();
        let mut cache = LayerCache::new(273);
        let query = vector::<Q>(123);
        let mut out = vec![0.0; Q];
        let mut scratch = Vec::new();
        for i in 0..600 {
            cache.append(i, &vector::<H>(i), &vector::<H>(i + 5));
            if [0, 63, 128, 255, 272, 273, 594].contains(&i) {
                single(&pool, &query, &cache, i, &mut out, &mut scratch);
                let expected = reference_f16(&cache, &query, i);
                for (a, b) in out.iter().zip(&expected) {
                    assert!((a - b).abs() < 1e-4, "pos={i}: got={a} expected={b}");
                }
            }
        }
    }

    #[test]
    fn prefill_parallel_rows_keep_causal_prefixes_across_evictions() {
        let pool = Pool::new(4).unwrap();
        let _guard = pool.enter();
        for first in [0, 64, 250, 535] {
            let mut cache = LayerCache::new(273);
            for i in 0..first {
                cache.append(i, &vector::<H>(i), &vector::<H>(i + 5));
            }
            let positions = (first..first + 37).collect::<Vec<_>>();
            let evicted = EvictedRows::capture(&cache, &positions);
            let mut qkv = Vec::new();
            let mut expected = Vec::new();
            for &i in &positions {
                cache.append(i, &vector::<H>(i), &vector::<H>(i + 5));
                let row = vector::<{ Q + 2 * H }>(i * 5);
                qkv.extend(row);
                expected.extend(reference_f16(&cache, &row[..Q], i));
            }
            let mut out = vec![0.0; expected.len()];
            prefill(&pool, &qkv, &cache, &positions, &evicted, &mut out);
            for (idx, (a, b)) in out.iter().zip(&expected).enumerate() {
                assert!((a - b).abs() < 1e-4, "first={first} idx={idx}: got={a} expected={b}");
            }
        }
    }
}
