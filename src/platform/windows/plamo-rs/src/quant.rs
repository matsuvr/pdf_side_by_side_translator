use crate::{
    Result, err,
    gguf::{Gguf, Tensor},
};
use rayon::prelude::*;
pub fn f16(x: u16) -> f32 {
    let sign = ((x as u32) & 0x8000) << 16;
    let exp = (x >> 10) & 31;
    let frac = (x & 1023) as u32;
    match exp {
        0 => {
            if frac == 0 {
                f32::from_bits(sign)
            } else {
                let v = frac as f32 * 2f32.powi(-24);
                if sign != 0 { -v } else { v }
            }
        }
        31 => f32::from_bits(sign | 0x7f800000 | (frac << 13)),
        _ => f32::from_bits(sign | (((exp as u32) + 112) << 23) | (frac << 13)),
    }
}
fn h(b: &[u8], i: usize) -> f32 {
    f16(u16::from_le_bytes([b[i], b[i + 1]]))
}
pub(crate) fn sm(b: &[u8], j: usize) -> (i32, i32) {
    if j < 4 {
        ((b[j] & 63) as i32, (b[j + 4] & 63) as i32)
    } else {
        (
            ((b[j + 4] & 15) | ((b[j - 4] >> 6) << 4)) as i32,
            ((b[j + 4] >> 4) | ((b[j] >> 6) << 4)) as i32,
        )
    }
}
#[derive(Clone)]
pub(crate) struct Q8 {
    pub(crate) d: f32,
    pub(crate) q: [i8; 256],
    pub(crate) sums: [i32; 16],
}
impl Q8 {
    pub(crate) fn new(x: &[f32]) -> Self {
        #[cfg(target_arch = "x86_64")]
        if std::is_x86_feature_detected!("avx2") {
            return unsafe { Self::avx(x) };
        }
        Self::scalar(x)
    }
    fn scalar(x: &[f32]) -> Self {
        let mut max = 0.0f32;
        for &v in x {
            if v.abs() > max.abs() {
                max = v;
            }
        }
        let inv = if max == 0.0 { 0.0 } else { -127.0 / max };
        let mut q = [0i8; 256];
        let mut sums = [0; 16];
        for i in 0..256 {
            q[i] = (x[i] * inv).round_ties_even().clamp(-128., 127.) as i8;
            sums[i / 16] += q[i] as i32;
        }
        Self {
            d: if inv == 0.0 { 0.0 } else { 1.0 / inv },
            q,
            sums,
        }
    }
    #[cfg(target_arch = "x86_64")]
    #[target_feature(enable = "avx2")]
    unsafe fn avx(x: &[f32]) -> Self {
        use std::arch::x86_64::*;
        unsafe {
            let mut max = _mm256_setzero_ps();
            let sign = _mm256_set1_ps(-0.);
            for i in (0..256).step_by(8) {
                max = _mm256_max_ps(
                    max,
                    _mm256_andnot_ps(sign, _mm256_loadu_ps(x.as_ptr().add(i))),
                );
            }
            let mut tmp = [0f32; 8];
            _mm256_storeu_ps(tmp.as_mut_ptr(), max);
            let absmax = tmp.iter().copied().fold(0., f32::max);
            let max = x.iter().copied().find(|v| v.abs() == absmax).unwrap_or(0.);
            let mut out = Self {
                d: 0.,
                q: [0; 256],
                sums: [0; 16],
            };
            if max == 0. {
                return out;
            }
            let inv = -127. / max;
            out.d = 1. / inv;
            let a = _mm256_set1_ps(inv);
            for i in (0..256).step_by(32) {
                let c = |j: usize| {
                    _mm256_cvttps_epi32(_mm256_round_ps::<
                        { _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC },
                    >(_mm256_mul_ps(
                        _mm256_loadu_ps(x.as_ptr().add(i + j)),
                        a,
                    )))
                };
                let p0 = _mm256_permute4x64_epi64::<0xD8>(_mm256_packs_epi32(c(0), c(8)));
                let p1 = _mm256_permute4x64_epi64::<0xD8>(_mm256_packs_epi32(c(16), c(24)));
                let p = _mm256_permute4x64_epi64::<0xD8>(_mm256_packs_epi16(p0, p1));
                _mm256_storeu_si256(out.q.as_mut_ptr().add(i).cast(), p);
                let ones8 = _mm_set1_epi8(1);
                let ones16 = _mm_set1_epi16(1);
                let sum16 = |v: __m128i| {
                    let s = _mm_madd_epi16(_mm_maddubs_epi16(ones8, v), ones16);
                    let s = _mm_add_epi32(s, _mm_srli_si128::<8>(s));
                    _mm_cvtsi128_si32(_mm_add_epi32(s, _mm_srli_si128::<4>(s)))
                };
                out.sums[i / 16] = sum16(_mm256_castsi256_si128(p));
                out.sums[i / 16 + 1] = sum16(_mm256_extracti128_si256::<1>(p));
            }
            out
        }
    }
}
type Dot = fn(&[u8], &[i8]) -> i32;
fn scalar(a: &[u8], b: &[i8]) -> i32 {
    a.iter().zip(b).map(|(&x, &y)| x as i32 * y as i32).sum()
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn dot32(
    a: std::arch::x86_64::__m256i,
    b: std::arch::x86_64::__m256i,
) -> std::arch::x86_64::__m256i {
    use std::arch::x86_64::*;
    _mm256_madd_epi16(_mm256_maddubs_epi16(a, b), _mm256_set1_epi16(1))
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn hsum(a: std::arch::x86_64::__m256i) -> i32 {
    use std::arch::x86_64::*;
    let mut out = [0i32; 8];
    unsafe { _mm256_storeu_si256(out.as_mut_ptr().cast(), a) };
    out.iter().sum()
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn q4_avx(b: &[u8], x: &Q8) -> f32 {
    use std::arch::x86_64::*;
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let mut acc = _mm256_setzero_si256();
        let mut mins = 0;
        for pair in 0..4 {
            let packed = _mm256_loadu_si256(b.as_ptr().add(16 + pair * 32).cast());
            for half in 0..2 {
                let j = pair * 2 + half;
                let q = if half == 0 {
                    _mm256_and_si256(packed, mask)
                } else {
                    _mm256_and_si256(_mm256_srli_epi16::<4>(packed), mask)
                };
                let y = _mm256_loadu_si256(x.q.as_ptr().add(j * 32).cast());
                let (s, m) = sm(&b[4..16], j);
                acc = _mm256_add_epi32(acc, _mm256_mullo_epi32(dot32(q, y), _mm256_set1_epi32(s)));
                mins += m * (x.sums[j * 2] + x.sums[j * 2 + 1]);
            }
        }
        x.d * (h(b, 0) * hsum(acc) as f32 - h(b, 2) * mins as f32)
    }
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn q6_avx(b: &[u8], x: &Q8) -> f32 {
    use std::arch::x86_64::*;
    unsafe {
        let mask4 = _mm256_set1_epi8(15);
        let mask2 = _mm256_set1_epi8(3);
        let mut acc = _mm256_setzero_si256();
        let mut correction = 0;
        for half in 0..2 {
            let lo0 = _mm256_loadu_si256(b.as_ptr().add(half * 64).cast());
            let lo1 = _mm256_loadu_si256(b.as_ptr().add(half * 64 + 32).cast());
            let hi = _mm256_loadu_si256(b.as_ptr().add(128 + half * 32).cast());
            for group in 0..4 {
                let lo = if group % 2 == 0 { lo0 } else { lo1 };
                let lo = if group < 2 {
                    _mm256_and_si256(lo, mask4)
                } else {
                    _mm256_and_si256(_mm256_srli_epi16::<4>(lo), mask4)
                };
                let hh = match group {
                    0 => hi,
                    1 => _mm256_srli_epi16::<2>(hi),
                    2 => _mm256_srli_epi16::<4>(hi),
                    _ => _mm256_srli_epi16::<6>(hi),
                };
                let q = _mm256_or_si256(lo, _mm256_slli_epi16::<4>(_mm256_and_si256(hh, mask2)));
                let j = half * 4 + group;
                let y = _mm256_loadu_si256(x.q.as_ptr().add(j * 32).cast());
                let s0 = b[192 + 2 * j] as i8 as i32;
                let s1 = b[193 + 2 * j] as i8 as i32;
                let weights = _mm256_setr_epi32(s0, s0, s0, s0, s1, s1, s1, s1);
                acc = _mm256_add_epi32(acc, _mm256_mullo_epi32(dot32(q, y), weights));
                correction += 32 * (s0 * x.sums[2 * j] + s1 * x.sums[2 * j + 1]);
            }
        }
        h(b, 208) * x.d * (hsum(acc) - correction) as f32
    }
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,avx512f,avx512vl,avx512vnni")]
unsafe fn q4_vnni(b: &[u8], x: &Q8) -> f32 {
    use std::arch::x86_64::*;
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let mut acc = _mm256_setzero_si256();
        let mut mins = 0;
        for pair in 0..4 {
            let packed = _mm256_loadu_si256(b.as_ptr().add(16 + pair * 32).cast());
            for half in 0..2 {
                let j = pair * 2 + half;
                let q = if half == 0 {
                    _mm256_and_si256(packed, mask)
                } else {
                    _mm256_and_si256(_mm256_srli_epi16::<4>(packed), mask)
                };
                let y = _mm256_loadu_si256(x.q.as_ptr().add(j * 32).cast());
                let (s, m) = sm(&b[4..16], j);
                acc = _mm256_add_epi32(
                    acc,
                    _mm256_mullo_epi32(
                        _mm256_dpbusd_epi32(_mm256_setzero_si256(), q, y),
                        _mm256_set1_epi32(s),
                    ),
                );
                mins += m * (x.sums[j * 2] + x.sums[j * 2 + 1]);
            }
        }
        x.d * (h(b, 0) * hsum(acc) as f32 - h(b, 2) * mins as f32)
    }
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,avx512f,avx512vl,avx512vnni")]
unsafe fn q6_vnni(b: &[u8], x: &Q8) -> f32 {
    use std::arch::x86_64::*;
    unsafe {
        let mask4 = _mm256_set1_epi8(15);
        let mask2 = _mm256_set1_epi8(3);
        let mut acc = _mm256_setzero_si256();
        let mut correction = 0;
        for half in 0..2 {
            let lo0 = _mm256_loadu_si256(b.as_ptr().add(half * 64).cast());
            let lo1 = _mm256_loadu_si256(b.as_ptr().add(half * 64 + 32).cast());
            let hi = _mm256_loadu_si256(b.as_ptr().add(128 + half * 32).cast());
            for group in 0..4 {
                let lo = if group % 2 == 0 { lo0 } else { lo1 };
                let lo = if group < 2 {
                    _mm256_and_si256(lo, mask4)
                } else {
                    _mm256_and_si256(_mm256_srli_epi16::<4>(lo), mask4)
                };
                let hh = match group {
                    0 => hi,
                    1 => _mm256_srli_epi16::<2>(hi),
                    2 => _mm256_srli_epi16::<4>(hi),
                    _ => _mm256_srli_epi16::<6>(hi),
                };
                let q = _mm256_or_si256(lo, _mm256_slli_epi16::<4>(_mm256_and_si256(hh, mask2)));
                let j = half * 4 + group;
                let y = _mm256_loadu_si256(x.q.as_ptr().add(j * 32).cast());
                let s0 = b[192 + 2 * j] as i8 as i32;
                let s1 = b[193 + 2 * j] as i8 as i32;
                let weights = _mm256_setr_epi32(s0, s0, s0, s0, s1, s1, s1, s1);
                acc = _mm256_add_epi32(
                    acc,
                    _mm256_mullo_epi32(_mm256_dpbusd_epi32(_mm256_setzero_si256(), q, y), weights),
                );
                correction += 32 * (s0 * x.sums[2 * j] + s1 * x.sums[2 * j + 1]);
            }
        }
        h(b, 208) * x.d * (hsum(acc) - correction) as f32
    }
}
type Kernel = fn(&[u8], &Q8) -> f32;
fn q4_scalar(b: &[u8], x: &Q8) -> f32 {
    q4_dot(b, x, scalar)
}
fn q6_scalar(b: &[u8], x: &Q8) -> f32 {
    q6_dot(b, x, scalar)
}
#[cfg(target_arch = "x86_64")]
fn q4_simd(b: &[u8], x: &Q8) -> f32 {
    unsafe { q4_avx(b, x) }
}
#[cfg(target_arch = "x86_64")]
fn q6_simd(b: &[u8], x: &Q8) -> f32 {
    unsafe { q6_avx(b, x) }
}
#[cfg(target_arch = "aarch64")]
#[target_feature(enable = "neon")]
unsafe fn neon_inner(a: &[u8], b: &[i8]) -> i32 {
    use std::arch::aarch64::*;
    unsafe {
        let mut i = 0;
        let mut sum = 0i32;
        while i + 16 <= a.len() {
            let x = vreinterpretq_s8_u8(vld1q_u8(a.as_ptr().add(i)));
            let y = vld1q_s8(b.as_ptr().add(i));
            sum += vaddlvq_s16(vmull_s8(vget_low_s8(x), vget_low_s8(y)));
            sum += vaddlvq_s16(vmull_s8(vget_high_s8(x), vget_high_s8(y)));
            i += 16;
        }
        sum + scalar(&a[i..], &b[i..])
    }
}
#[cfg(target_arch = "aarch64")]
fn neon_dot(a: &[u8], b: &[i8]) -> i32 {
    unsafe { neon_inner(a, b) }
}
#[cfg(target_arch = "aarch64")]
fn q4_neon(b: &[u8], x: &Q8) -> f32 {
    q4_dot(b, x, neon_dot)
}
#[cfg(target_arch = "aarch64")]
fn q6_neon(b: &[u8], x: &Q8) -> f32 {
    q6_dot(b, x, neon_dot)
}
#[cfg(target_arch = "x86_64")]
fn q4_vnni_dispatch(b: &[u8], x: &Q8) -> f32 {
    unsafe { q4_vnni(b, x) }
}
#[cfg(target_arch = "x86_64")]
fn q6_vnni_dispatch(b: &[u8], x: &Q8) -> f32 {
    unsafe { q6_vnni(b, x) }
}
fn kernel(ty: u32) -> Kernel {
    #[cfg(target_arch = "x86_64")]
    if !cfg!(feature = "force-avx2")
        && std::is_x86_feature_detected!("avx512vnni")
        && std::is_x86_feature_detected!("avx512vl")
        && std::is_x86_feature_detected!("avx512f")
        && std::is_x86_feature_detected!("avx2")
    {
        return if ty == 12 {
            q4_vnni_dispatch
        } else {
            q6_vnni_dispatch
        };
    }
    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2") {
        return if ty == 12 { q4_simd } else { q6_simd };
    }
    #[cfg(target_arch = "aarch64")]
    if std::arch::is_aarch64_feature_detected!("neon") {
        return if ty == 12 { q4_neon } else { q6_neon };
    }
    if ty == 12 { q4_scalar } else { q6_scalar }
}
fn q4_dot(b: &[u8], x: &Q8, dot: Dot) -> f32 {
    let d = h(b, 0);
    let m = h(b, 2);
    let s = &b[4..16];
    let qs = &b[16..];
    let mut sum = 0;
    let mut mins = 0;
    let mut q = [0u8; 32];
    for j in 0..8 {
        let (scale, minimum) = sm(s, j);
        let start = (j / 2) * 32;
        for i in 0..32 {
            q[i] = if j % 2 == 0 {
                qs[start + i] & 15
            } else {
                qs[start + i] >> 4
            };
        }
        sum += scale * dot(&q, &x.q[j * 32..j * 32 + 32]);
        mins += minimum * (x.sums[j * 2] + x.sums[j * 2 + 1]);
    }
    x.d * (d * sum as f32 - m * mins as f32)
}
pub(crate) fn q6_value(b: &[u8], i: usize) -> u8 {
    let half = i / 128;
    let r = i % 128;
    let group = r / 32;
    let l = r % 32;
    let ql = b[half * 64 + (group % 2) * 32 + l];
    let low = if group < 2 { ql & 15 } else { ql >> 4 };
    low | (((b[128 + half * 32 + l] >> (group * 2)) & 3) << 4)
}
fn q6_dot(b: &[u8], x: &Q8, dot: Dot) -> f32 {
    let mut sum = 0;
    let mut q = [0u8; 32];
    for j in 0..8 {
        for i in 0..32 {
            q[i] = q6_value(b, j * 32 + i);
        }
        for half in 0..2 {
            let n = j * 2 + half;
            let scale = b[192 + n] as i8 as i32;
            sum += scale
                * (dot(&q[half * 16..half * 16 + 16], &x.q[n * 16..n * 16 + 16]) - 32 * x.sums[n]);
        }
    }
    h(b, 208) * x.d * sum as f32
}
pub fn row(g: &Gguf, t: &Tensor, index: usize) -> Result<Vec<f32>> {
    let n = t.dims[0];
    let data = g.data(t);
    let bs = match t.ty {
        0 => 4 * n,
        12 => 144 * n / 256,
        14 => 210 * n / 256,
        _ => return Err(err("bad row type")),
    };
    let start = index.checked_mul(bs).ok_or_else(|| err("row overflow"))?;
    let b = data
        .get(start..start + bs)
        .ok_or_else(|| err("row out of bounds"))?;
    let mut out = Vec::with_capacity(n);
    match t.ty {
        0 => out.extend(
            b.chunks_exact(4)
                .map(|x| f32::from_le_bytes(x.try_into().unwrap())),
        ),
        12 => {
            for b in b.chunks_exact(144) {
                for j in 0..8 {
                    let (s, m) = sm(&b[4..16], j);
                    for i in 0..32 {
                        let q = b[16 + j / 2 * 32 + i];
                        let q = if j % 2 == 0 { q & 15 } else { q >> 4 };
                        out.push(h(b, 0) * s as f32 * q as f32 - h(b, 2) * m as f32);
                    }
                }
            }
        }
        14 => {
            for b in b.chunks_exact(210) {
                for i in 0..256 {
                    out.push(
                        h(b, 208) * b[192 + i / 16] as i8 as f32 * (q6_value(b, i) as f32 - 32.),
                    );
                }
            }
        }
        _ => unreachable!(),
    }
    Ok(out)
}
pub fn matvec(g: &Gguf, t: &Tensor, x: &[f32], out: &mut [f32]) {
    let n = t.dims[0];
    assert_eq!(x.len(), n);
    assert_eq!(out.len(), t.dims[1]);
    let data = g.data(t);
    let q = x.chunks_exact(256).map(Q8::new).collect::<Vec<_>>();
    let dot = kernel(t.ty);
    let block = if t.ty == 12 { 144 } else { 210 };
    out.par_iter_mut()
        .zip(data.par_chunks_exact(n / 256 * block))
        .for_each(|(o, row)| {
            *o = row
                .chunks_exact(block)
                .zip(&q)
                .map(|(b, x)| dot(b, x))
                .sum();
        });
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,avx512f,avx512bw,avx512vl,avx512vnni")]
unsafe fn batch_block_vnni(
    b: &[u8],
    q: &[Q8],
    block_index: usize,
    blocks: usize,
    out: &mut [f32],
    ty: u32,
) {
    use std::arch::x86_64::*;
    unsafe {
        let mask4 = _mm256_set1_epi8(15);
        let mut weights = [_mm512_setzero_si512(); 4];
        let mut scales = [_mm512_setzero_si512(); 4];
        let mut corr = [0i32; 16];
        let (d, m) = if ty == 12 {
            for j in 0..4 {
                let pack = _mm256_loadu_si256(b.as_ptr().add(16 + 32 * j).cast());
                let lo = _mm256_and_si256(pack, mask4);
                let hi = _mm256_and_si256(_mm256_srli_epi16::<4>(pack), mask4);
                weights[j] = _mm512_inserti64x4::<1>(_mm512_castsi256_si512(lo), hi);
                let (s0, m0) = sm(&b[4..16], 2 * j);
                let (s1, m1) = sm(&b[4..16], 2 * j + 1);
                scales[j] = _mm512_setr_epi32(
                    s0, s0, s0, s0, s0, s0, s0, s0, s1, s1, s1, s1, s1, s1, s1, s1,
                );
                corr[4 * j..4 * j + 4].copy_from_slice(&[m0, m0, m1, m1]);
            }
            (h(b, 0), h(b, 2))
        } else {
            let mask2 = _mm256_set1_epi8(3);
            for half in 0..2 {
                let lo0 = _mm256_loadu_si256(b.as_ptr().add(half * 64).cast());
                let lo1 = _mm256_loadu_si256(b.as_ptr().add(half * 64 + 32).cast());
                let hi = _mm256_loadu_si256(b.as_ptr().add(128 + half * 32).cast());
                let hh = [
                    hi,
                    _mm256_srli_epi16::<2>(hi),
                    _mm256_srli_epi16::<4>(hi),
                    _mm256_srli_epi16::<6>(hi),
                ];
                let mut vals = [_mm256_setzero_si256(); 4];
                for group in 0..4 {
                    let lo = if group % 2 == 0 { lo0 } else { lo1 };
                    let lo = if group < 2 {
                        _mm256_and_si256(lo, mask4)
                    } else {
                        _mm256_and_si256(_mm256_srli_epi16::<4>(lo), mask4)
                    };
                    vals[group] = _mm256_or_si256(
                        lo,
                        _mm256_slli_epi16::<4>(_mm256_and_si256(hh[group], mask2)),
                    );
                }
                for j in 0..2 {
                    let k = half * 2 + j;
                    weights[k] = _mm512_inserti64x4::<1>(
                        _mm512_castsi256_si512(vals[j * 2]),
                        vals[j * 2 + 1],
                    );
                    let s0 = b[192 + 4 * k] as i8 as i32;
                    let s1 = b[193 + 4 * k] as i8 as i32;
                    let s2 = b[194 + 4 * k] as i8 as i32;
                    let s3 = b[195 + 4 * k] as i8 as i32;
                    scales[k] = _mm512_setr_epi32(
                        s0, s0, s0, s0, s1, s1, s1, s1, s2, s2, s2, s2, s3, s3, s3, s3,
                    );
                    corr[4 * k..4 * k + 4].copy_from_slice(&[s0 * 32, s1 * 32, s2 * 32, s3 * 32]);
                }
            }
            (h(b, 208), h(b, 208))
        };
        let correction = _mm512_loadu_si512(corr.as_ptr().cast());
        for (j, o) in out.iter_mut().enumerate() {
            let x = &q[j * blocks + block_index];
            let mut acc = _mm512_setzero_si512();
            for group in 0..4 {
                let y = _mm512_loadu_si512(x.q.as_ptr().add(group * 64).cast());
                let dot = _mm512_dpbusd_epi32(_mm512_setzero_si512(), weights[group], y);
                acc = _mm512_add_epi32(acc, _mm512_mullo_epi32(dot, scales[group]));
            }
            let sums = _mm512_loadu_si512(x.sums.as_ptr().cast());
            let c = _mm512_reduce_add_epi32(_mm512_mullo_epi32(sums, correction));
            let v = _mm512_reduce_add_epi32(acc);
            *o += if ty == 12 {
                x.d * (d * v as f32 - m * c as f32)
            } else {
                d * x.d * (v - c) as f32
            };
        }
    }
}
#[cfg(target_arch = "x86_64")]
fn has_batch_vnni() -> bool {
    if cfg!(feature = "force-avx2") {
        return false;
    }
    #[cfg(target_arch = "x86_64")]
    {
        return std::is_x86_feature_detected!("avx512vnni")
            && std::is_x86_feature_detected!("avx512vl")
            && std::is_x86_feature_detected!("avx512bw")
            && std::is_x86_feature_detected!("avx512f")
            && std::is_x86_feature_detected!("avx2");
    }
    #[allow(unreachable_code)]
    false
}
/// Token-major input and output. Weight rows are kept hot across the batch.
pub fn matmul_batch(g: &Gguf, t: &Tensor, x: &[f32], out: &mut [f32], tokens: usize) {
    let n = t.dims[0];
    let m = t.dims[1];
    let blocks = n / 256;
    assert_eq!(x.len(), tokens * n);
    assert_eq!(out.len(), tokens * m);
    let block = if t.ty == 12 { 144 } else { 210 };
    let q = x.chunks_exact(256).map(Q8::new).collect::<Vec<_>>();
    let data = g.data(t);
    let dot = kernel(t.ty);
    let mut row_major = vec![0f32; m * tokens];
    #[cfg(target_arch = "x86_64")]
    let vnni = has_batch_vnni();
    row_major
        .par_chunks_exact_mut(tokens)
        .zip(data.par_chunks_exact(blocks * block))
        .for_each(|(o, row)| {
            for (i, b) in row.chunks_exact(block).enumerate() {
                #[cfg(target_arch = "x86_64")]
                if vnni {
                    unsafe { batch_block_vnni(b, &q, i, blocks, o, t.ty) };
                    continue;
                }
                for (j, v) in o.iter_mut().enumerate() {
                    *v += dot(b, &q[j * blocks + i]);
                }
            }
        });
    out.par_chunks_exact_mut(m).enumerate().for_each(|(i, o)| {
        for (r, v) in o.iter_mut().enumerate() {
            *v = row_major[r * tokens + i];
        }
    });
}
pub(crate) fn best_q6_panels(data: &[u8], x: &[f32], panels: &[usize]) -> u32 {
    let q = x.chunks_exact(256).map(Q8::new).collect::<Vec<_>>();
    let kernel = kernel(14);
    let row_size = q.len() * 210;
    panels
        .par_iter()
        .flat_map_iter(|&panel| {
            (panel * 8..panel * 8 + 8).map(|row| {
                let score = data[row * row_size..(row + 1) * row_size]
                    .chunks_exact(210)
                    .zip(&q)
                    .map(|(b, x)| kernel(b, x))
                    .sum::<f32>();
                (score, row as u32)
            })
        })
        .reduce(
            || (f32::NEG_INFINITY, u32::MAX),
            |a, b| {
                if b.0 > a.0 || (b.0 == a.0 && b.1 < a.1) {
                    b
                } else {
                    a
                }
            },
        )
        .1
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn half() {
        assert_eq!(f16(0x3c00), 1.0);
        assert_eq!(f16(0xc000), -2.0);
        assert_eq!(f16(1), 2f32.powi(-24));
    }
    #[test]
    fn dot_paths() {
        let x = Q8::new(
            &(0..256)
                .map(|i| (i as f32 * 0.31).sin())
                .collect::<Vec<_>>(),
        );
        let y = Q8::scalar(
            &(0..256)
                .map(|i| (i as f32 * 0.31).sin())
                .collect::<Vec<_>>(),
        );
        assert_eq!(x.q, y.q);
        assert_eq!(x.d, y.d);
        assert_eq!(x.sums, y.sums);
        let mut b4 = (0..144).map(|i| (i * 79 + 17) as u8).collect::<Vec<_>>();
        b4[0..4].copy_from_slice(&[0, 60, 0, 56]);
        let mut b6 = (0..210).map(|i| (i * 71 + 47) as u8).collect::<Vec<_>>();
        b6[208..210].copy_from_slice(&[0, 60]);
        assert_eq!(q4_scalar(&b4, &x), kernel(12)(&b4, &x));
        assert_eq!(q6_scalar(&b6, &x), kernel(14)(&b6, &x));
    }
}
