//! P4X86 v7 embedding/head quantization and runtime kernels.
use crate::{
    Result, err,
    pool::Pool,
    weights::{Tensor, Weights},
};

pub const Q4: u32 = 101;
pub const BLOCK: usize = 32;
pub const BYTES: usize = 20;

#[inline]
pub fn bf16(v: &[u8]) -> f32 {
    f32::from_bits((u16::from_le_bytes([v[0], v[1]]) as u32) << 16)
}

pub(crate) fn quantize(raw: &[u8]) -> [u8; BYTES] {
    let vals: [f32; BLOCK] = std::array::from_fn(|i| bf16(&raw[2 * i..]));
    let max = vals.iter().map(|x| x.abs()).fold(0., f32::max);
    let d = max / 7.;
    let mut out = [0u8; BYTES];
    out[..4].copy_from_slice(&d.to_le_bytes());
    for i in 0..16 {
        // signed -> biased; compute in i32 before cast
        let q = |j: usize| {
            if d == 0. {
                8
            } else {
                ((vals[j] / d).round_ties_even().clamp(-7., 7.) as i32 + 8) as u8
            }
        };
        out[4 + i] = q(i) | (q(i + 16) << 4);
    }
    out
}

pub fn row_bytes(_ty: u32, n: usize) -> usize {
    n / BLOCK * BYTES
}

pub fn row_into(g: &Weights, t: &Tensor, i: usize, dst: &mut [f32]) -> Result<()> {
    let n = t.dims[0];
    if dst.len() != n || i >= t.dims[1] {
        return Err(err("row range"));
    }
    if t.ty == crate::model::TILE {
        let groups = n / 32;
        let a = (i / 8) * groups * 160;
        let r = i % 8;
        let mut idx = 0;
        for b in g.data(t)[a..a + groups * 160].chunks_exact(160) {
            let d = f32::from_le_bytes(b[r * 4..r * 4 + 4].try_into()?);
            for j in 0..32 {
                let k = j % 16;
                let byte = b[(k / 4 + 1) * 32 + r * 4 + k % 4];
                let v = if j < 16 { byte & 15 } else { byte >> 4 };
                dst[idx] = d * (v as f32 - 8.);
                idx += 1;
            }
        }
        return Ok(());
    }
    let size = row_bytes(t.ty, n);
    let b = g
        .data(t)
        .get(i * size..(i + 1) * size)
        .ok_or_else(|| err("row range"))?;
    let mut idx = 0;
    for blk in b.chunks_exact(BYTES) {
        let d = f32::from_le_bytes(blk[..4].try_into()?);
        for j in 0..16 {
            let byte = blk[4 + j];
            dst[idx + j] = d * ((byte & 15) as f32 - 8.);
            dst[idx + 16 + j] = d * ((byte >> 4) as f32 - 8.);
        }
        idx += 32;
    }
    Ok(())
}

#[allow(dead_code)]
pub fn row(g: &Weights, t: &Tensor, i: usize) -> Result<Vec<f32>> {
    let mut out = vec![0.0f32; t.dims[0]];
    row_into(g, t, i, &mut out)?;
    Ok(out)
}

#[repr(C, align(32))]
#[derive(Clone, Copy)]
pub(crate) struct Q8 {
    pub(crate) d: f32,
    pub(crate) neg_8_sum: i32,
    pub(crate) vals: [i8; 32],
}

impl Default for Q8 {
    fn default() -> Self {
        Self {
            d: 0.0,
            neg_8_sum: 0,
            vals: [0; 32],
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn q8_avx2(x: &[f32]) -> Q8 {
    use std::arch::x86_64::*;
    debug_assert_eq!(x.len(), 32);
    unsafe {
        let sign = _mm256_set1_ps(-0.0);
        let xp = x.as_ptr();
        let x0 = _mm256_loadu_ps(xp);
        let x1 = _mm256_loadu_ps(xp.add(8));
        let x2 = _mm256_loadu_ps(xp.add(16));
        let x3 = _mm256_loadu_ps(xp.add(24));

        let m01 = _mm256_max_ps(_mm256_andnot_ps(sign, x0), _mm256_andnot_ps(sign, x1));
        let m23 = _mm256_max_ps(_mm256_andnot_ps(sign, x2), _mm256_andnot_ps(sign, x3));
        let mv = _mm256_max_ps(m01, m23);
        let m128 = _mm_max_ps(_mm256_castps256_ps128(mv), _mm256_extractf128_ps::<1>(mv));
        let m128 = _mm_max_ps(m128, _mm_movehl_ps(m128, m128));
        let m128 = _mm_max_ss(m128, _mm_shuffle_ps::<1>(m128, m128));
        let max_abs = _mm_cvtss_f32(m128);
        if max_abs == 0.0 {
            return Q8::default();
        }
        let d = max_abs / 127.0;
        let v_inv = _mm256_set1_ps(127.0 / max_abs);

        let q0 = _mm256_cvtps_epi32(_mm256_round_ps::<{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }>(_mm256_mul_ps(x0, v_inv)));
        let q1 = _mm256_cvtps_epi32(_mm256_round_ps::<{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }>(_mm256_mul_ps(x1, v_inv)));
        let q2 = _mm256_cvtps_epi32(_mm256_round_ps::<{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }>(_mm256_mul_ps(x2, v_inv)));
        let q3 = _mm256_cvtps_epi32(_mm256_round_ps::<{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }>(_mm256_mul_ps(x3, v_inv)));

        let p0 = _mm256_permute4x64_epi64::<0xD8>(_mm256_packs_epi32(q0, q1));
        let p1 = _mm256_permute4x64_epi64::<0xD8>(_mm256_packs_epi32(q2, q3));
        let p8 = _mm256_permute4x64_epi64::<0xD8>(_mm256_packs_epi16(p0, p1));

        let mut vals = [0i8; 32];
        _mm256_storeu_si256(vals.as_mut_ptr().cast(), p8);

        let s16 = _mm256_maddubs_epi16(_mm256_set1_epi8(1), p8);
        let s32 = _mm256_madd_epi16(s16, _mm256_set1_epi16(1));
        let s128 = _mm_add_epi32(_mm256_castsi256_si128(s32), _mm256_extracti128_si256::<1>(s32));
        let s128 = _mm_add_epi32(s128, _mm_srli_si128::<8>(s128));
        let s128 = _mm_add_epi32(s128, _mm_srli_si128::<4>(s128));
        let sum = _mm_cvtsi128_si32(s128);
        Q8 {
            d,
            neg_8_sum: -8 * sum,
            vals,
        }
    }
}

pub(crate) fn q8_one(x: &[f32]) -> Q8 {
    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2") {
        return unsafe { q8_avx2(x) };
    }
    let max_abs = x.iter().map(|v| v.abs()).fold(0.0f32, f32::max);
    if max_abs == 0.0 {
        return Q8::default();
    }
    let d = max_abs / 127.0;
    let inv = 127.0 / max_abs;
    let vals = std::array::from_fn(|i| (x[i] * inv).round_ties_even().clamp(-127., 127.) as i8);
    let sum: i32 = vals.iter().map(|&v| v as i32).sum();
    Q8 {
        d,
        neg_8_sum: -8 * sum,
        vals,
    }
}

pub(crate) fn quantize_q8_slice(x: &[f32], out: &mut [Q8]) {
    for (src, dst) in x.chunks_exact(32).zip(out.iter_mut()) {
        *dst = q8_one(src);
    }
}

#[derive(Clone, Copy)]
pub(crate) enum HeadIsa {
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
pub(crate) fn select_head_isa() -> HeadIsa {
    #[cfg(target_arch = "x86_64")]
    {
        if std::is_x86_feature_detected!("avx2") {
            let fma = std::is_x86_feature_detected!("fma");
            if !cfg!(feature = "force-avx2")
                && fma
                && std::is_x86_feature_detected!("avx512vnni")
                && std::is_x86_feature_detected!("avx512vl")
            {
                return HeadIsa::Avx512Vnni;
            }
            if !cfg!(feature = "force-avx2") && fma && std::is_x86_feature_detected!("avxvnni") {
                return HeadIsa::AvxVnniFma;
            }
            if fma {
                return HeadIsa::Avx2Fma;
            }
            return HeadIsa::Avx2;
        }
    }
    HeadIsa::Scalar
}

#[inline]
pub(crate) fn score_panel_w4(isa: HeadIsa, tile: &[u8], q: &[Q8], dst: &mut [f32; 8]) {
    match isa {
        #[cfg(target_arch = "x86_64")]
        HeadIsa::Avx512Vnni => unsafe { tile8_vnni_one(tile, q, dst) },
        #[cfg(target_arch = "x86_64")]
        HeadIsa::AvxVnniFma => unsafe { tile8_avxvnni_one(tile, q, dst) },
        #[cfg(target_arch = "x86_64")]
        HeadIsa::Avx2Fma => unsafe { tile8_fma_one(tile, q, dst) },
        #[cfg(target_arch = "x86_64")]
        HeadIsa::Avx2 => unsafe { tile8_avx2_one(tile, q, dst) },
        HeadIsa::Scalar => tile8_scalar_one(tile, q, dst),
    }
}

#[inline]
pub(crate) fn score_2panels_w4(
    isa: HeadIsa,
    t0: &[u8],
    t1: &[u8],
    q: &[Q8],
    d0: &mut [f32; 8],
    d1: &mut [f32; 8],
) {
    match isa {
        #[cfg(target_arch = "x86_64")]
        HeadIsa::Avx512Vnni => unsafe { tile8_vnni_two(t0, t1, q, d0, d1) },
        #[cfg(target_arch = "x86_64")]
        HeadIsa::AvxVnniFma => unsafe { tile8_avxvnni_two(t0, t1, q, d0, d1) },
        #[cfg(target_arch = "x86_64")]
        HeadIsa::Avx2Fma => unsafe { tile8_fma_two(t0, t1, q, d0, d1) },
        _ => {
            score_panel_w4(isa, t0, q, d0);
            score_panel_w4(isa, t1, q, d1);
        }
    }
}

fn tile8_scalar_one(tile: &[u8], q: &[Q8], out: &mut [f32]) {
    out.fill(0.0);
    for (b, v) in tile.chunks_exact(160).zip(q) {
        for r in 0..8 {
            let d = f32::from_le_bytes(b[r * 4..r * 4 + 4].try_into().unwrap());
            let mut s = v.neg_8_sum;
            for k in 0..16 {
                let p = b[(k / 4 + 1) * 32 + r * 4 + k % 4];
                s += (p & 15) as i32 * v.vals[k] as i32 + (p >> 4) as i32 * v.vals[k + 16] as i32;
            }
            out[r] += d * v.d * s as f32;
        }
    }
}

pub fn multiply_tile8_pooled(
    pool: &Pool,
    g: &Weights,
    t: &Tensor,
    x: &[f32],
    out: &mut [f32],
    count: usize,
) {
    let n = t.dims[0];
    let m = t.dims[1];
    let groups = n / 32;
    let mut q = vec![Q8::default(); count * groups];
    quantize_q8_slice(x, &mut q);
    let data = g.data(t);
    let isa = select_head_isa();
    let n_tiles = m / 8;
    let tile_bytes = groups * 160;
    let out_addr = out.as_mut_ptr() as usize;
    let nth = if count == 1 {
        pool.decode_threads()
    } else {
        pool.num_threads()
    }
    .min(n_tiles)
    .max(1);

    pool.dispatch(nth, |ith, nth| {
        let t_start = ith * n_tiles / nth;
        let t_end = (ith + 1) * n_tiles / nth;
        let out_ptr = out_addr as *mut f32;
        for tile_idx in t_start..t_end {
            let tile = &data[tile_idx * tile_bytes..(tile_idx + 1) * tile_bytes];
            for j in 0..count {
                let qj = &q[j * groups..(j + 1) * groups];
                let mut dst = [0.0f32; 8];
                score_panel_w4(isa, tile, qj, &mut dst);
                unsafe {
                    std::ptr::copy_nonoverlapping(dst.as_ptr(), out_ptr.add(j * m + tile_idx * 8), 8);
                }
            }
        }
    });
}

#[cfg(test)]
pub fn multiply_tile8(g: &Weights, t: &Tensor, x: &[f32], out: &mut [f32], count: usize) {
    let pool = Pool::new(2).unwrap();
    multiply_tile8_pooled(&pool, g, t, x, out, count);
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn tile8_avx2_one(tile: &[u8], q: &[Q8], out: &mut [f32]) {
    use std::arch::x86_64::*;
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let ones = _mm256_set1_epi16(1);
        let mut acc = _mm256_setzero_ps();
        for (b, x) in tile.chunks_exact(160).zip(q) {
            let dw = _mm256_loadu_ps(b.as_ptr().cast());
            let mut pair = _mm256_setzero_si256();
            for k in 0..4 {
                let p = _mm256_loadu_si256(b.as_ptr().add(32 + k * 32).cast());
                let lo = _mm256_and_si256(p, mask);
                let hi = _mm256_and_si256(_mm256_srli_epi16::<4>(p), mask);
                let a = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(k * 4).cast::<i32>(),
                ));
                let c = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(16 + k * 4).cast::<i32>(),
                ));
                pair = _mm256_add_epi16(pair, _mm256_maddubs_epi16(lo, a));
                pair = _mm256_add_epi16(pair, _mm256_maddubs_epi16(hi, c));
            }
            let sum = _mm256_add_epi32(_mm256_madd_epi16(pair, ones), _mm256_set1_epi32(x.neg_8_sum));
            acc = _mm256_add_ps(
                acc,
                _mm256_mul_ps(_mm256_cvtepi32_ps(sum), _mm256_mul_ps(dw, _mm256_set1_ps(x.d))),
            );
        }
        _mm256_storeu_ps(out.as_mut_ptr().cast(), acc);
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
unsafe fn tile8_fma_one(tile: &[u8], q: &[Q8], out: &mut [f32]) {
    use std::arch::x86_64::*;
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let ones = _mm256_set1_epi16(1);
        let mut acc = _mm256_setzero_ps();
        for (b, x) in tile.chunks_exact(160).zip(q) {
            let dw = _mm256_loadu_ps(b.as_ptr().cast());
            let mut pairlo = _mm256_setzero_si256();
            let mut pairhi = _mm256_setzero_si256();
            for k in 0..4 {
                let p = _mm256_loadu_si256(b.as_ptr().add(32 + k * 32).cast());
                let lo = _mm256_and_si256(p, mask);
                let hi = _mm256_and_si256(_mm256_srli_epi16::<4>(p), mask);
                let a = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(k * 4).cast::<i32>(),
                ));
                let c = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(16 + k * 4).cast::<i32>(),
                ));
                pairlo = _mm256_add_epi16(pairlo, _mm256_maddubs_epi16(lo, a));
                pairhi = _mm256_add_epi16(pairhi, _mm256_maddubs_epi16(hi, c));
            }
            let sum = _mm256_add_epi32(
                _mm256_madd_epi16(_mm256_add_epi16(pairlo, pairhi), ones),
                _mm256_set1_epi32(x.neg_8_sum),
            );
            acc = _mm256_fmadd_ps(
                _mm256_cvtepi32_ps(sum),
                _mm256_mul_ps(dw, _mm256_set1_ps(x.d)),
                acc,
            );
        }
        _mm256_storeu_ps(out.as_mut_ptr().cast(), acc);
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma,avxvnni")]
unsafe fn tile8_avxvnni_one(tile: &[u8], q: &[Q8], out: &mut [f32]) {
    use std::arch::x86_64::*;
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let mut acc = _mm256_setzero_ps();
        for (b, x) in tile.chunks_exact(160).zip(q) {
            let dw = _mm256_loadu_ps(b.as_ptr().cast());
            let mut sum_lo = _mm256_set1_epi32(x.neg_8_sum);
            let mut sum_hi = _mm256_setzero_si256();
            for k in 0..4 {
                let p = _mm256_loadu_si256(b.as_ptr().add(32 + k * 32).cast());
                let lo = _mm256_and_si256(p, mask);
                let hi = _mm256_and_si256(_mm256_srli_epi16::<4>(p), mask);
                let a = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(k * 4).cast::<i32>(),
                ));
                let c = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(16 + k * 4).cast::<i32>(),
                ));
                sum_lo = _mm256_dpbusd_avx_epi32(sum_lo, lo, a);
                sum_hi = _mm256_dpbusd_avx_epi32(sum_hi, hi, c);
            }
            let sum = _mm256_add_epi32(sum_lo, sum_hi);
            acc = _mm256_fmadd_ps(
                _mm256_cvtepi32_ps(sum),
                _mm256_mul_ps(dw, _mm256_set1_ps(x.d)),
                acc,
            );
        }
        _mm256_storeu_ps(out.as_mut_ptr().cast(), acc);
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma,avx512vnni,avx512vl")]
unsafe fn tile8_vnni_one(tile: &[u8], q: &[Q8], out: &mut [f32]) {
    use std::arch::x86_64::*;
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let mut acc = _mm256_setzero_ps();
        for (b, x) in tile.chunks_exact(160).zip(q) {
            let mut sum_lo = _mm256_set1_epi32(x.neg_8_sum);
            let mut sum_hi = _mm256_setzero_si256();
            for k in 0..4 {
                let p = _mm256_loadu_si256(b.as_ptr().add(32 + k * 32).cast());
                let lo = _mm256_and_si256(p, mask);
                let hi = _mm256_and_si256(_mm256_srli_epi16::<4>(p), mask);
                let a = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(k * 4).cast::<i32>(),
                ));
                let c = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(16 + k * 4).cast::<i32>(),
                ));
                sum_lo = _mm256_dpbusd_epi32(sum_lo, lo, a);
                sum_hi = _mm256_dpbusd_epi32(sum_hi, hi, c);
            }
            let dw = _mm256_loadu_ps(b.as_ptr().cast());
            let sum = _mm256_add_epi32(sum_lo, sum_hi);
            acc = _mm256_fmadd_ps(
                _mm256_cvtepi32_ps(sum),
                _mm256_mul_ps(dw, _mm256_set1_ps(x.d)),
                acc,
            );
        }
        _mm256_storeu_ps(out.as_mut_ptr().cast(), acc);
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma,avxvnni")]
unsafe fn tile8_avxvnni_two(t0: &[u8], t1: &[u8], q: &[Q8], o0: &mut [f32], o1: &mut [f32]) {
    use std::arch::x86_64::*;
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let mut acc0 = _mm256_setzero_ps();
        let mut acc1 = _mm256_setzero_ps();
        for ((b0, b1), x) in t0.chunks_exact(160).zip(t1.chunks_exact(160)).zip(q) {
            let mut lo0 = _mm256_set1_epi32(x.neg_8_sum);
            let mut hi0 = _mm256_setzero_si256();
            let mut lo1 = lo0;
            let mut hi1 = _mm256_setzero_si256();
            for k in 0..4 {
                let a = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(k * 4).cast::<i32>(),
                ));
                let c = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(16 + k * 4).cast::<i32>(),
                ));
                let p0 = _mm256_loadu_si256(b0.as_ptr().add(32 + k * 32).cast());
                lo0 = _mm256_dpbusd_avx_epi32(lo0, _mm256_and_si256(p0, mask), a);
                hi0 = _mm256_dpbusd_avx_epi32(
                    hi0,
                    _mm256_and_si256(_mm256_srli_epi16::<4>(p0), mask),
                    c,
                );
                let p1 = _mm256_loadu_si256(b1.as_ptr().add(32 + k * 32).cast());
                lo1 = _mm256_dpbusd_avx_epi32(lo1, _mm256_and_si256(p1, mask), a);
                hi1 = _mm256_dpbusd_avx_epi32(
                    hi1,
                    _mm256_and_si256(_mm256_srli_epi16::<4>(p1), mask),
                    c,
                );
            }
            let da = _mm256_set1_ps(x.d);
            let dw0 = _mm256_loadu_ps(b0.as_ptr().cast());
            let dw1 = _mm256_loadu_ps(b1.as_ptr().cast());
            acc0 = _mm256_fmadd_ps(
                _mm256_cvtepi32_ps(_mm256_add_epi32(lo0, hi0)),
                _mm256_mul_ps(dw0, da),
                acc0,
            );
            acc1 = _mm256_fmadd_ps(
                _mm256_cvtepi32_ps(_mm256_add_epi32(lo1, hi1)),
                _mm256_mul_ps(dw1, da),
                acc1,
            );
        }
        _mm256_storeu_ps(o0.as_mut_ptr().cast(), acc0);
        _mm256_storeu_ps(o1.as_mut_ptr().cast(), acc1);
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma,avx512vnni,avx512vl")]
unsafe fn tile8_vnni_two(t0: &[u8], t1: &[u8], q: &[Q8], o0: &mut [f32], o1: &mut [f32]) {
    use std::arch::x86_64::*;
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let mut acc0 = _mm256_setzero_ps();
        let mut acc1 = _mm256_setzero_ps();
        for ((b0, b1), x) in t0.chunks_exact(160).zip(t1.chunks_exact(160)).zip(q) {
            let mut lo0 = _mm256_set1_epi32(x.neg_8_sum);
            let mut hi0 = _mm256_setzero_si256();
            let mut lo1 = lo0;
            let mut hi1 = _mm256_setzero_si256();
            for k in 0..4 {
                let a = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(k * 4).cast::<i32>(),
                ));
                let c = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(16 + k * 4).cast::<i32>(),
                ));
                let p0 = _mm256_loadu_si256(b0.as_ptr().add(32 + k * 32).cast());
                lo0 = _mm256_dpbusd_epi32(lo0, _mm256_and_si256(p0, mask), a);
                hi0 = _mm256_dpbusd_epi32(
                    hi0,
                    _mm256_and_si256(_mm256_srli_epi16::<4>(p0), mask),
                    c,
                );
                let p1 = _mm256_loadu_si256(b1.as_ptr().add(32 + k * 32).cast());
                lo1 = _mm256_dpbusd_epi32(lo1, _mm256_and_si256(p1, mask), a);
                hi1 = _mm256_dpbusd_epi32(
                    hi1,
                    _mm256_and_si256(_mm256_srli_epi16::<4>(p1), mask),
                    c,
                );
            }
            let da = _mm256_set1_ps(x.d);
            let dw0 = _mm256_loadu_ps(b0.as_ptr().cast());
            let dw1 = _mm256_loadu_ps(b1.as_ptr().cast());
            acc0 = _mm256_fmadd_ps(
                _mm256_cvtepi32_ps(_mm256_add_epi32(lo0, hi0)),
                _mm256_mul_ps(dw0, da),
                acc0,
            );
            acc1 = _mm256_fmadd_ps(
                _mm256_cvtepi32_ps(_mm256_add_epi32(lo1, hi1)),
                _mm256_mul_ps(dw1, da),
                acc1,
            );
        }
        _mm256_storeu_ps(o0.as_mut_ptr().cast(), acc0);
        _mm256_storeu_ps(o1.as_mut_ptr().cast(), acc1);
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
unsafe fn tile8_fma_two(t0: &[u8], t1: &[u8], q: &[Q8], o0: &mut [f32], o1: &mut [f32]) {
    use std::arch::x86_64::*;
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let ones = _mm256_set1_epi16(1);
        let mut acc0 = _mm256_setzero_ps();
        let mut acc1 = _mm256_setzero_ps();
        for ((b0, b1), x) in t0.chunks_exact(160).zip(t1.chunks_exact(160)).zip(q) {
            let mut pl0 = _mm256_setzero_si256();
            let mut ph0 = _mm256_setzero_si256();
            let mut pl1 = _mm256_setzero_si256();
            let mut ph1 = _mm256_setzero_si256();
            for k in 0..4 {
                let a = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(k * 4).cast::<i32>(),
                ));
                let c = _mm256_set1_epi32(std::ptr::read_unaligned(
                    x.vals.as_ptr().add(16 + k * 4).cast::<i32>(),
                ));
                let p0 = _mm256_loadu_si256(b0.as_ptr().add(32 + k * 32).cast());
                pl0 = _mm256_add_epi16(pl0, _mm256_maddubs_epi16(_mm256_and_si256(p0, mask), a));
                ph0 = _mm256_add_epi16(
                    ph0,
                    _mm256_maddubs_epi16(_mm256_and_si256(_mm256_srli_epi16::<4>(p0), mask), c),
                );
                let p1 = _mm256_loadu_si256(b1.as_ptr().add(32 + k * 32).cast());
                pl1 = _mm256_add_epi16(pl1, _mm256_maddubs_epi16(_mm256_and_si256(p1, mask), a));
                ph1 = _mm256_add_epi16(
                    ph1,
                    _mm256_maddubs_epi16(_mm256_and_si256(_mm256_srli_epi16::<4>(p1), mask), c),
                );
            }
            let bias = _mm256_set1_epi32(x.neg_8_sum);
            let s0 = _mm256_add_epi32(_mm256_madd_epi16(_mm256_add_epi16(pl0, ph0), ones), bias);
            let s1 = _mm256_add_epi32(_mm256_madd_epi16(_mm256_add_epi16(pl1, ph1), ones), bias);
            let da = _mm256_set1_ps(x.d);
            acc0 = _mm256_fmadd_ps(_mm256_cvtepi32_ps(s0), _mm256_mul_ps(_mm256_loadu_ps(b0.as_ptr().cast()), da), acc0);
            acc1 = _mm256_fmadd_ps(_mm256_cvtepi32_ps(s1), _mm256_mul_ps(_mm256_loadu_ps(b1.as_ptr().cast()), da), acc1);
        }
        _mm256_storeu_ps(o0.as_mut_ptr().cast(), acc0);
        _mm256_storeu_ps(o1.as_mut_ptr().cast(), acc1);
    }
}
