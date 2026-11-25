//! Model-specialized runtime packing. Compressed Q4_K codes are interleaved
//! across output rows so SIMD lanes hold independent outputs, not fragments of one dot.
use crate::gguf::{Gguf, Tensor};
#[cfg(target_arch = "x86_64")]
use crate::quant::{Q8, f16, q6_value, sm};
#[cfg(target_arch = "x86_64")]
use rayon::prelude::*;

#[cfg(not(target_arch = "x86_64"))]
pub struct PackedMatrix;
#[cfg(not(target_arch = "x86_64"))]
impl PackedMatrix {
    pub fn build(_g: &Gguf, _t: &Tensor) -> Option<Self> {
        None
    }
    pub fn multiply(&self, _x: &[f32], _out: &mut [f32], _tokens: usize) {
        unreachable!("packing requires x86_64")
    }
}

#[cfg(target_arch = "x86_64")]
pub struct PackedMatrix {
    n: usize,
    m: usize,
    data: Vec<u8>,
    width: usize,
    ty: u32,
}
#[cfg(target_arch = "x86_64")]
impl PackedMatrix {
    pub fn build(g: &Gguf, t: &Tensor) -> Option<Self> {
        #[cfg(target_arch = "x86_64")]
        if std::is_x86_feature_detected!("avx2") && [12, 14].contains(&t.ty) && t.dims.len() == 2 {
            let n = t.dims[0];
            let m = t.dims[1];
            let width = if vnni512() { 16 } else { 8 };
            if m % width != 0 {
                return None;
            }
            let bs = n / 256;
            let size = width * if t.ty == 12 { 152 } else { 276 };
            let data = g.data(t);
            let mut packed = vec![0u8; m / width * bs * size];
            packed
                .par_chunks_exact_mut(bs * size)
                .enumerate()
                .for_each(|(tile, dst)| {
                    for block in 0..bs {
                        let d = &mut dst[block * size..(block + 1) * size];
                        for r in 0..width {
                            if t.ty == 12 {
                                let off = ((tile * width + r) * bs + block) * 144;
                                let b = &data[off..off + 144];
                                d[r * 4..r * 4 + 4].copy_from_slice(
                                    &f16(u16::from_le_bytes([b[0], b[1]])).to_le_bytes(),
                                );
                                let p = 4 * width + r * 4;
                                d[p..p + 4].copy_from_slice(
                                    &f16(u16::from_le_bytes([b[2], b[3]])).to_le_bytes(),
                                );
                                for j in 0..8 {
                                    let (s, m) = sm(&b[4..16], j);
                                    d[8 * width + j * width + r] = s as u8;
                                    d[16 * width + j * width + r] = m as u8;
                                }
                                for k in 0..32 {
                                    let off = (24 + 4 * k) * width + r * 4;
                                    d[off..off + 4].copy_from_slice(&b[16 + 4 * k..20 + 4 * k]);
                                }
                            } else {
                                let off = ((tile * width + r) * bs + block) * 210;
                                let b = &data[off..off + 210];
                                d[r * 4..r * 4 + 4].copy_from_slice(
                                    &f16(u16::from_le_bytes([b[208], b[209]])).to_le_bytes(),
                                );
                                for j in 0..16 {
                                    d[(4 + j) * width + r] = b[192 + j];
                                }
                                for k in 0..64 {
                                    let off = (20 + 4 * k) * width + r * 4;
                                    for j in 0..4 {
                                        d[off + j] = q6_value(b, k * 4 + j);
                                    }
                                }
                            }
                        }
                    }
                });
            return Some(Self {
                n,
                m,
                data: packed,
                width,
                ty: t.ty,
            });
        }
        let _ = (g, t);
        None
    }
    pub fn multiply(&self, x: &[f32], out: &mut [f32], tokens: usize) {
        let blocks = self.n / 256;
        let width = self.width;
        let size = width * if self.ty == 12 { 152 } else { 276 };
        let q = x.chunks_exact(256).map(Q8::new).collect::<Vec<_>>();
        assert_eq!(q.len(), tokens * blocks);
        assert_eq!(out.len(), tokens * self.m);
        if tokens == 1 {
            out.par_chunks_exact_mut(width)
                .zip(self.data.par_chunks_exact(blocks * size))
                .for_each(|(o, tile)| {
                    #[cfg(target_arch = "x86_64")]
                    unsafe {
                        if self.ty == 12 {
                            if width == 16 {
                                decode16(tile, &q, blocks, o, 12);
                            } else {
                                tile256(tile, &q, blocks, o, 1);
                            }
                        } else if width == 16 {
                            decode16(tile, &q, blocks, o, 14);
                        } else {
                            tile6_256(tile, &q, blocks, o, 1);
                        }
                    }
                });
        } else {
            let mut by_tile = vec![0.; out.len()];
            by_tile
                .par_chunks_exact_mut(width * tokens)
                .zip(self.data.par_chunks_exact(blocks * size))
                .for_each(|(o, tile)| {
                    #[cfg(target_arch = "x86_64")]
                    unsafe {
                        if self.ty == 12 {
                            if width == 16 {
                                tile4_batch512(tile, &q, blocks, o, tokens);
                            } else {
                                tile256(tile, &q, blocks, o, tokens);
                            }
                        } else if width == 16 {
                            tile6_batch512(tile, &q, blocks, o, tokens);
                        } else {
                            tile6_256(tile, &q, blocks, o, tokens);
                        }
                    }
                });
            out.par_chunks_exact_mut(self.m)
                .enumerate()
                .for_each(|(i, o)| {
                    for (tile, vals) in o.chunks_exact_mut(width).enumerate() {
                        let start = (tile * tokens + i) * width;
                        vals.copy_from_slice(&by_tile[start..start + width]);
                    }
                });
        }
    }
}
#[cfg(target_arch = "x86_64")]
fn vnni512() -> bool {
    if cfg!(feature = "force-avx2") {
        return false;
    }
    #[cfg(target_arch = "x86_64")]
    {
        return std::is_x86_feature_detected!("avx512f")
            && std::is_x86_feature_detected!("avx512bw")
            && std::is_x86_feature_detected!("avx512vnni")
            && std::is_x86_feature_detected!("avx512vl")
            && std::is_x86_feature_detected!("fma");
    }
    #[allow(unreachable_code)]
    false
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn tile256(data: &[u8], q: &[Q8], blocks: usize, out: &mut [f32], tokens: usize) {
    use std::arch::x86_64::*;
    unsafe {
        const W: usize = 8;
        const SIZE: usize = W * 152;
        let mask = _mm256_set1_epi8(15);
        let ones = _mm256_set1_epi16(1);
        for block in 0..blocks {
            let b = data.as_ptr().add(block * SIZE);
            let d = _mm256_loadu_ps(b.cast());
            let m = _mm256_loadu_ps(b.add(W * 4).cast());
            for token in 0..tokens {
                let x = &q[token * blocks + block];
                let mut sum = _mm256_setzero_si256();
                let mut mins = _mm256_setzero_si256();
                for pair in 0..4 {
                    let mut lo = _mm256_setzero_si256();
                    let mut hi = lo;
                    for k in 0..8 {
                        let w = _mm256_loadu_si256(b.add((24 + 4 * (pair * 8 + k)) * W).cast());
                        let i = pair * 64 + k * 4;
                        let xl = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.q.as_ptr().add(i).cast::<i32>(),
                        ));
                        let xh = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.q.as_ptr().add(i + 32).cast::<i32>(),
                        ));
                        lo = _mm256_add_epi32(
                            lo,
                            _mm256_madd_epi16(
                                _mm256_maddubs_epi16(_mm256_and_si256(w, mask), xl),
                                ones,
                            ),
                        );
                        hi = _mm256_add_epi32(
                            hi,
                            _mm256_madd_epi16(
                                _mm256_maddubs_epi16(
                                    _mm256_and_si256(_mm256_srli_epi16::<4>(w), mask),
                                    xh,
                                ),
                                ones,
                            ),
                        );
                    }
                    for j in 0..2 {
                        let g = pair * 2 + j;
                        let scale =
                            _mm256_cvtepu8_epi32(_mm_loadl_epi64(b.add((8 + g) * W).cast()));
                        let minimum =
                            _mm256_cvtepu8_epi32(_mm_loadl_epi64(b.add((16 + g) * W).cast()));
                        sum = _mm256_add_epi32(
                            sum,
                            _mm256_mullo_epi32(if j == 0 { lo } else { hi }, scale),
                        );
                        mins = _mm256_add_epi32(
                            mins,
                            _mm256_mullo_epi32(
                                minimum,
                                _mm256_set1_epi32(x.sums[2 * g] + x.sums[2 * g + 1]),
                            ),
                        );
                    }
                }
                let result = _mm256_mul_ps(
                    _mm256_set1_ps(x.d),
                    _mm256_sub_ps(
                        _mm256_mul_ps(d, _mm256_cvtepi32_ps(sum)),
                        _mm256_mul_ps(m, _mm256_cvtepi32_ps(mins)),
                    ),
                );
                let o = out.as_mut_ptr().add(token * W);
                _mm256_storeu_ps(o, _mm256_add_ps(_mm256_loadu_ps(o), result));
            }
        }
    }
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,avx512f,avx512bw,avx512vnni")]
unsafe fn tile512(data: &[u8], q: &[Q8], blocks: usize, out: &mut [f32], tokens: usize) {
    use std::arch::x86_64::*;
    unsafe {
        const W: usize = 16;
        const SIZE: usize = W * 152;
        let mask = _mm512_set1_epi8(15);
        for block in 0..blocks {
            let b = data.as_ptr().add(block * SIZE);
            let d = _mm512_loadu_ps(b.cast());
            let m = _mm512_loadu_ps(b.add(W * 4).cast());
            for token in 0..tokens {
                let x = &q[token * blocks + block];
                let mut sum = _mm512_setzero_si512();
                let mut mins = _mm512_setzero_si512();
                for pair in 0..4 {
                    let mut lo = _mm512_setzero_si512();
                    let mut hi = lo;
                    for k in 0..8 {
                        let w = _mm512_loadu_si512(b.add((24 + 4 * (pair * 8 + k)) * W).cast());
                        let i = pair * 64 + k * 4;
                        let xl = _mm512_set1_epi32(std::ptr::read_unaligned(
                            x.q.as_ptr().add(i).cast::<i32>(),
                        ));
                        let xh = _mm512_set1_epi32(std::ptr::read_unaligned(
                            x.q.as_ptr().add(i + 32).cast::<i32>(),
                        ));
                        lo = _mm512_dpbusd_epi32(lo, _mm512_and_si512(w, mask), xl);
                        hi = _mm512_dpbusd_epi32(
                            hi,
                            _mm512_and_si512(_mm512_srli_epi16::<4>(w), mask),
                            xh,
                        );
                    }
                    for j in 0..2 {
                        let g = pair * 2 + j;
                        let scale =
                            _mm512_cvtepu8_epi32(_mm_loadu_si128(b.add((8 + g) * W).cast()));
                        let minimum =
                            _mm512_cvtepu8_epi32(_mm_loadu_si128(b.add((16 + g) * W).cast()));
                        sum = _mm512_add_epi32(
                            sum,
                            _mm512_mullo_epi32(if j == 0 { lo } else { hi }, scale),
                        );
                        mins = _mm512_add_epi32(
                            mins,
                            _mm512_mullo_epi32(
                                minimum,
                                _mm512_set1_epi32(x.sums[2 * g] + x.sums[2 * g + 1]),
                            ),
                        );
                    }
                }
                let result = _mm512_mul_ps(
                    _mm512_set1_ps(x.d),
                    _mm512_sub_ps(
                        _mm512_mul_ps(d, _mm512_cvtepi32_ps(sum)),
                        _mm512_mul_ps(m, _mm512_cvtepi32_ps(mins)),
                    ),
                );
                let o = out.as_mut_ptr().add(token * W);
                _mm512_storeu_ps(o, _mm512_add_ps(_mm512_loadu_ps(o), result));
            }
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn tile6_256(data: &[u8], q: &[Q8], blocks: usize, out: &mut [f32], tokens: usize) {
    use std::arch::x86_64::*;
    unsafe {
        const W: usize = 8;
        const SIZE: usize = W * 276;
        let ones = _mm256_set1_epi16(1);
        for block in 0..blocks {
            let b = data.as_ptr().add(block * SIZE);
            let d = _mm256_loadu_ps(b.cast());
            for token in 0..tokens {
                let x = &q[token * blocks + block];
                let mut sum = _mm256_setzero_si256();
                for group in 0..16 {
                    let mut acc = _mm256_setzero_si256();
                    for j in 0..4 {
                        let k = group * 4 + j;
                        let w = _mm256_loadu_si256(b.add((20 + k * 4) * W).cast());
                        let a = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.q.as_ptr().add(k * 4).cast::<i32>(),
                        ));
                        acc = _mm256_add_epi32(
                            acc,
                            _mm256_madd_epi16(_mm256_maddubs_epi16(w, a), ones),
                        );
                    }
                    let scale =
                        _mm256_cvtepi8_epi32(_mm_loadl_epi64(b.add((4 + group) * W).cast()));
                    acc = _mm256_sub_epi32(acc, _mm256_set1_epi32(32 * x.sums[group]));
                    sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(acc, scale));
                }
                let result = _mm256_mul_ps(
                    _mm256_mul_ps(d, _mm256_set1_ps(x.d)),
                    _mm256_cvtepi32_ps(sum),
                );
                let o = out.as_mut_ptr().add(token * W);
                _mm256_storeu_ps(o, _mm256_add_ps(_mm256_loadu_ps(o), result));
            }
        }
    }
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,avx512f,avx512bw,avx512vnni")]
unsafe fn tile6_512(data: &[u8], q: &[Q8], blocks: usize, out: &mut [f32], tokens: usize) {
    use std::arch::x86_64::*;
    unsafe {
        const W: usize = 16;
        const SIZE: usize = W * 276;
        for block in 0..blocks {
            let b = data.as_ptr().add(block * SIZE);
            let d = _mm512_loadu_ps(b.cast());
            for token in 0..tokens {
                let x = &q[token * blocks + block];
                let mut sum = _mm512_setzero_si512();
                for group in 0..16 {
                    let mut acc = _mm512_setzero_si512();
                    for j in 0..4 {
                        let k = group * 4 + j;
                        let w = _mm512_loadu_si512(b.add((20 + k * 4) * W).cast());
                        let a = _mm512_set1_epi32(std::ptr::read_unaligned(
                            x.q.as_ptr().add(k * 4).cast::<i32>(),
                        ));
                        acc = _mm512_dpbusd_epi32(acc, w, a);
                    }
                    let scale =
                        _mm512_cvtepi8_epi32(_mm_loadu_si128(b.add((4 + group) * W).cast()));
                    acc = _mm512_sub_epi32(acc, _mm512_set1_epi32(32 * x.sums[group]));
                    sum = _mm512_add_epi32(sum, _mm512_mullo_epi32(acc, scale));
                }
                let result = _mm512_mul_ps(
                    _mm512_mul_ps(d, _mm512_set1_ps(x.d)),
                    _mm512_cvtepi32_ps(sum),
                );
                let o = out.as_mut_ptr().add(token * W);
                _mm512_storeu_ps(o, _mm512_add_ps(_mm512_loadu_ps(o), result));
            }
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,avx512f,avx512bw,avx512vnni")]
unsafe fn tile4_batch512(data: &[u8], q: &[Q8], blocks: usize, out: &mut [f32], tokens: usize) {
    use std::arch::x86_64::*;
    unsafe {
        const W: usize = 16;
        const SIZE: usize = W * 152;
        const N: usize = 4;
        let mask = _mm512_set1_epi8(15);
        for block in 0..blocks {
            let b = data.as_ptr().add(block * SIZE);
            let d = _mm512_loadu_ps(b.cast());
            let m = _mm512_loadu_ps(b.add(W * 4).cast());
            for base in (0..tokens / N * N).step_by(N) {
                let mut sum = [_mm512_setzero_si512(); N];
                let mut mins = sum;
                for pair in 0..4 {
                    let mut lo = [_mm512_setzero_si512(); N];
                    let mut hi = lo;
                    for k in 0..8 {
                        let w = _mm512_loadu_si512(b.add((24 + 4 * (pair * 8 + k)) * W).cast());
                        let wl = _mm512_and_si512(w, mask);
                        let wh = _mm512_and_si512(_mm512_srli_epi16::<4>(w), mask);
                        let i = pair * 64 + k * 4;
                        for t in 0..N {
                            let x = &q[(base + t) * blocks + block];
                            let xl = _mm512_set1_epi32(std::ptr::read_unaligned(
                                x.q.as_ptr().add(i).cast::<i32>(),
                            ));
                            let xh = _mm512_set1_epi32(std::ptr::read_unaligned(
                                x.q.as_ptr().add(i + 32).cast::<i32>(),
                            ));
                            lo[t] = _mm512_dpbusd_epi32(lo[t], wl, xl);
                            hi[t] = _mm512_dpbusd_epi32(hi[t], wh, xh);
                        }
                    }
                    for j in 0..2 {
                        let g = pair * 2 + j;
                        let scale =
                            _mm512_cvtepu8_epi32(_mm_loadu_si128(b.add((8 + g) * W).cast()));
                        let minimum =
                            _mm512_cvtepu8_epi32(_mm_loadu_si128(b.add((16 + g) * W).cast()));
                        for t in 0..N {
                            let x = &q[(base + t) * blocks + block];
                            sum[t] = _mm512_add_epi32(
                                sum[t],
                                _mm512_mullo_epi32(if j == 0 { lo[t] } else { hi[t] }, scale),
                            );
                            mins[t] = _mm512_add_epi32(
                                mins[t],
                                _mm512_mullo_epi32(
                                    minimum,
                                    _mm512_set1_epi32(x.sums[2 * g] + x.sums[2 * g + 1]),
                                ),
                            );
                        }
                    }
                }
                for t in 0..N {
                    let x = &q[(base + t) * blocks + block];
                    let result = _mm512_mul_ps(
                        _mm512_set1_ps(x.d),
                        _mm512_sub_ps(
                            _mm512_mul_ps(d, _mm512_cvtepi32_ps(sum[t])),
                            _mm512_mul_ps(m, _mm512_cvtepi32_ps(mins[t])),
                        ),
                    );
                    let o = out.as_mut_ptr().add((base + t) * W);
                    _mm512_storeu_ps(o, _mm512_add_ps(_mm512_loadu_ps(o), result));
                }
            }
        }
        if tokens % N != 0 {
            let done = tokens / N * N;
            tile512(
                data,
                &q[done * blocks..],
                blocks,
                &mut out[done * W..],
                tokens - done,
            );
        }
    }
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,avx512f,avx512bw,avx512vnni")]
unsafe fn tile6_batch512(data: &[u8], q: &[Q8], blocks: usize, out: &mut [f32], tokens: usize) {
    use std::arch::x86_64::*;
    unsafe {
        const W: usize = 16;
        const SIZE: usize = W * 276;
        const N: usize = 4;
        for block in 0..blocks {
            let b = data.as_ptr().add(block * SIZE);
            let d = _mm512_loadu_ps(b.cast());
            for base in (0..tokens / N * N).step_by(N) {
                let mut sum = [_mm512_setzero_si512(); N];
                for group in 0..16 {
                    let mut acc = [_mm512_setzero_si512(); N];
                    for j in 0..4 {
                        let k = group * 4 + j;
                        let w = _mm512_loadu_si512(b.add((20 + k * 4) * W).cast());
                        for t in 0..N {
                            let x = &q[(base + t) * blocks + block];
                            let a = _mm512_set1_epi32(std::ptr::read_unaligned(
                                x.q.as_ptr().add(k * 4).cast::<i32>(),
                            ));
                            acc[t] = _mm512_dpbusd_epi32(acc[t], w, a);
                        }
                    }
                    let scale =
                        _mm512_cvtepi8_epi32(_mm_loadu_si128(b.add((4 + group) * W).cast()));
                    for t in 0..N {
                        let x = &q[(base + t) * blocks + block];
                        acc[t] = _mm512_sub_epi32(acc[t], _mm512_set1_epi32(32 * x.sums[group]));
                        sum[t] = _mm512_add_epi32(sum[t], _mm512_mullo_epi32(acc[t], scale));
                    }
                }
                for t in 0..N {
                    let x = &q[(base + t) * blocks + block];
                    let result = _mm512_mul_ps(
                        _mm512_mul_ps(d, _mm512_set1_ps(x.d)),
                        _mm512_cvtepi32_ps(sum[t]),
                    );
                    let o = out.as_mut_ptr().add((base + t) * W);
                    _mm512_storeu_ps(o, _mm512_add_ps(_mm512_loadu_ps(o), result));
                }
            }
        }
        if tokens % N != 0 {
            let done = tokens / N * N;
            tile6_512(
                data,
                &q[done * blocks..],
                blocks,
                &mut out[done * W..],
                tokens - done,
            );
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma,avx512f,avx512vl,avx512vnni")]
unsafe fn decode16(data: &[u8], q: &[Q8], blocks: usize, out: &mut [f32], ty: u32) {
    use std::arch::x86_64::*;
    unsafe {
        const W: usize = 16;
        let size = W * if ty == 12 { 152 } else { 276 };
        let mask = _mm256_set1_epi8(15);
        let zero = _mm256_setzero_si256();
        let mut out0 = _mm256_setzero_ps();
        let mut out1 = out0;
        for block in 0..blocks {
            let b = data.as_ptr().add(block * size);
            let x = &q[block];
            let xd = _mm256_set1_ps(x.d);
            if block + 1 < blocks {
                let p = b.add(size);
                _mm_prefetch(p.cast(), _MM_HINT_T0);
                _mm_prefetch(p.add(512).cast(), _MM_HINT_T0);
                _mm_prefetch(p.add(1024).cast(), _MM_HINT_T0);
                _mm_prefetch(p.add(1536).cast(), _MM_HINT_T0);
            }
            let d0 = _mm256_loadu_ps(b.cast());
            let d1 = _mm256_loadu_ps(b.add(32).cast());
            let mut sum0 = zero;
            let mut sum1 = zero;
            if ty == 12 {
                let mut min0 = zero;
                let mut min1 = zero;
                for pair in 0..4 {
                    let (mut lo0, mut lo1, mut hi0, mut hi1) = (zero, zero, zero, zero);
                    for k in 0..8 {
                        let ptr = b.add((24 + 4 * (pair * 8 + k)) * W);
                        let i = pair * 64 + k * 4;
                        let a0 = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.q.as_ptr().add(i).cast::<i32>(),
                        ));
                        let a1 = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.q.as_ptr().add(i + 32).cast::<i32>(),
                        ));
                        let w0 = _mm256_loadu_si256(ptr.cast());
                        let w1 = _mm256_loadu_si256(ptr.add(32).cast());
                        lo0 = _mm256_dpbusd_epi32(lo0, _mm256_and_si256(w0, mask), a0);
                        lo1 = _mm256_dpbusd_epi32(lo1, _mm256_and_si256(w1, mask), a0);
                        hi0 = _mm256_dpbusd_epi32(
                            hi0,
                            _mm256_and_si256(_mm256_srli_epi16::<4>(w0), mask),
                            a1,
                        );
                        hi1 = _mm256_dpbusd_epi32(
                            hi1,
                            _mm256_and_si256(_mm256_srli_epi16::<4>(w1), mask),
                            a1,
                        );
                    }
                    for j in 0..2 {
                        let g = pair * 2 + j;
                        let xs = _mm256_set1_epi32(x.sums[2 * g] + x.sums[2 * g + 1]);
                        let sc = b.add((8 + g) * W);
                        let mn = b.add((16 + g) * W);
                        let sc0 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(sc.cast()));
                        let sc1 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(sc.add(8).cast()));
                        let mn0 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(mn.cast()));
                        let mn1 = _mm256_cvtepu8_epi32(_mm_loadl_epi64(mn.add(8).cast()));
                        sum0 = _mm256_add_epi32(
                            sum0,
                            _mm256_mullo_epi32(if j == 0 { lo0 } else { hi0 }, sc0),
                        );
                        sum1 = _mm256_add_epi32(
                            sum1,
                            _mm256_mullo_epi32(if j == 0 { lo1 } else { hi1 }, sc1),
                        );
                        min0 = _mm256_add_epi32(min0, _mm256_mullo_epi32(mn0, xs));
                        min1 = _mm256_add_epi32(min1, _mm256_mullo_epi32(mn1, xs));
                    }
                }
                let m0 = _mm256_loadu_ps(b.add(4 * W).cast());
                let m1 = _mm256_loadu_ps(b.add(4 * W + 32).cast());
                out0 = _mm256_add_ps(
                    out0,
                    _mm256_mul_ps(
                        xd,
                        _mm256_sub_ps(
                            _mm256_mul_ps(d0, _mm256_cvtepi32_ps(sum0)),
                            _mm256_mul_ps(m0, _mm256_cvtepi32_ps(min0)),
                        ),
                    ),
                );
                out1 = _mm256_add_ps(
                    out1,
                    _mm256_mul_ps(
                        xd,
                        _mm256_sub_ps(
                            _mm256_mul_ps(d1, _mm256_cvtepi32_ps(sum1)),
                            _mm256_mul_ps(m1, _mm256_cvtepi32_ps(min1)),
                        ),
                    ),
                );
            } else {
                for group in 0..16 {
                    let mut a0 = zero;
                    let mut a1 = zero;
                    for j in 0..4 {
                        let k = group * 4 + j;
                        let a = _mm256_set1_epi32(std::ptr::read_unaligned(
                            x.q.as_ptr().add(k * 4).cast::<i32>(),
                        ));
                        let ptr = b.add((20 + k * 4) * W);
                        a0 = _mm256_dpbusd_epi32(a0, _mm256_loadu_si256(ptr.cast()), a);
                        a1 = _mm256_dpbusd_epi32(a1, _mm256_loadu_si256(ptr.add(32).cast()), a);
                    }
                    let sc = b.add((4 + group) * W);
                    let xs = _mm256_set1_epi32(32 * x.sums[group]);
                    let sc0 = _mm256_cvtepi8_epi32(_mm_loadl_epi64(sc.cast()));
                    let sc1 = _mm256_cvtepi8_epi32(_mm_loadl_epi64(sc.add(8).cast()));
                    sum0 =
                        _mm256_add_epi32(sum0, _mm256_mullo_epi32(_mm256_sub_epi32(a0, xs), sc0));
                    sum1 =
                        _mm256_add_epi32(sum1, _mm256_mullo_epi32(_mm256_sub_epi32(a1, xs), sc1));
                }
                out0 = _mm256_add_ps(
                    out0,
                    _mm256_mul_ps(_mm256_mul_ps(d0, xd), _mm256_cvtepi32_ps(sum0)),
                );
                out1 = _mm256_add_ps(
                    out1,
                    _mm256_mul_ps(_mm256_mul_ps(d1, xd), _mm256_cvtepi32_ps(sum1)),
                );
            }
        }
        _mm256_storeu_ps(out.as_mut_ptr(), out0);
        _mm256_storeu_ps(out.as_mut_ptr().add(8), out1);
    }
}
