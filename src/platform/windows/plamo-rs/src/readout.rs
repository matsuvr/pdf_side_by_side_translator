//! PLaMo output-head specialization: 1-bit sign search, then original Q6_K
//! scoring on a global shortlist. Derived from the same two-stage design used
//! in the user's existing PLaMo CPU experiment; the shortlist is approximate.
use crate::quant;
#[cfg(target_arch = "x86_64")]
use crate::quant::{f16, q6_value};
use rayon::prelude::*;
const GROUPS: usize = 3360;
const ROWS: usize = 32;
const BLOCKS: usize = 6;
const PANELS: usize = GROUPS * 4;
const SHORTLIST: usize = 384;
struct Group {
    bits: Box<[u8]>,
    scales: [[f32; ROWS]; BLOCKS],
}
pub struct Readout {
    groups: Vec<Group>,
}
struct Lut {
    values: [[[i8; 16]; 64]; BLOCKS],
    scales: [f32; BLOCKS],
}
impl Lut {
    fn new(x: &[f32]) -> Self {
        let mut r = Self {
            values: [[[0; 16]; 64]; BLOCKS],
            scales: [0.; BLOCKS],
        };
        for b in 0..BLOCKS {
            let x = &x[b * 256..(b + 1) * 256];
            let max = x
                .chunks_exact(4)
                .map(|g| g.iter().map(|v| v.abs()).sum::<f32>())
                .fold(0., f32::max);
            r.scales[b] = max / 31.;
            let inv = if max > 0. { 31. / max } else { 0. };
            for (j, g) in x.chunks_exact(4).enumerate() {
                for mask in 0..16 {
                    let v = (0..4)
                        .map(|i| if mask & (1 << i) != 0 { g[i] } else { -g[i] })
                        .sum::<f32>()
                        * inv;
                    r.values[b][j][mask] = v.round_ties_even().clamp(-31., 31.) as i8;
                }
            }
        }
        r
    }
}
impl Readout {
    pub fn build(data: &[u8]) -> Option<Self> {
        #[cfg(target_arch = "x86_64")]
        if std::is_x86_feature_detected!("avx2") {
            assert_eq!(data.len(), GROUPS * ROWS * BLOCKS * 210);
            let groups = data
                .par_chunks_exact(ROWS * BLOCKS * 210)
                .map(|src| {
                    let mut g = Group {
                        bits: vec![0; BLOCKS * ROWS * 32].into_boxed_slice(),
                        scales: [[0.; ROWS]; BLOCKS],
                    };
                    for row in 0..ROWS {
                        for b in 0..BLOCKS {
                            let off = (row * BLOCKS + b) * 210;
                            let blk = &src[off..off + 210];
                            let d = f16(u16::from_le_bytes([blk[208], blk[209]]));
                            let mut sum = 0f32;
                            for k in 0..32 {
                                let mut mask = 0u8;
                                for i in 0..8 {
                                    let n = k * 8 + i;
                                    let w = d
                                        * blk[192 + n / 16] as i8 as f32
                                        * (q6_value(blk, n) as f32 - 32.);
                                    sum += w.abs();
                                    if w >= 0. {
                                        mask |= 1 << i;
                                    }
                                }
                                g.bits[(b * 32 + k) * ROWS + row] = mask;
                            }
                            g.scales[b][row] = sum / 256.;
                        }
                    }
                    g
                })
                .collect();
            return Some(Self { groups });
        }
        let _ = data;
        None
    }
    pub fn best(&self, weights: &[u8], x: &[f32]) -> u32 {
        let lut = Lut::new(x);
        let mut panels: Vec<(f32, usize)> = self
            .groups
            .par_iter()
            .enumerate()
            .flat_map_iter(|(i, g)| {
                let scores = approximate(g, &lut);
                (0..4).map(move |p| {
                    (
                        scores[p * 8..p * 8 + 8]
                            .iter()
                            .copied()
                            .fold(f32::NEG_INFINITY, f32::max),
                        i * 4 + p,
                    )
                })
            })
            .collect();
        debug_assert_eq!(panels.len(), PANELS);
        // Stable deterministic order for tied approximate scores.
        let cmp = |a: &(f32, usize), b: &(f32, usize)| b.0.total_cmp(&a.0).then(a.1.cmp(&b.1));
        panels.select_nth_unstable_by(SHORTLIST, cmp);
        let mut selected = panels[..SHORTLIST]
            .iter()
            .map(|&(_, id)| id)
            .collect::<Vec<_>>();
        selected.sort_unstable();
        quant::best_q6_panels(weights, x, &selected)
    }
    pub fn best_many(&self, weights: &[u8], x: &[f32], count: usize) -> Vec<u32> {
        if count == 1 {
            return vec![self.best(weights, x)];
        }
        assert_eq!(x.len(), 1536 * count);
        let luts = x.chunks_exact(1536).map(Lut::new).collect::<Vec<_>>();
        let mut values = vec![0f32; PANELS * count];
        values
            .par_chunks_exact_mut(4 * count)
            .zip(self.groups.par_iter())
            .for_each(|(dst, g)| {
                for (j, lut) in luts.iter().enumerate() {
                    let scores = approximate(g, lut);
                    for p in 0..4 {
                        dst[p * count + j] = scores[p * 8..p * 8 + 8]
                            .iter()
                            .copied()
                            .fold(f32::NEG_INFINITY, f32::max);
                    }
                }
            });
        (0..count)
            .into_par_iter()
            .map(|i| {
                let mut panels = (0..PANELS)
                    .map(|p| (values[p * count + i], p))
                    .collect::<Vec<_>>();
                let cmp =
                    |a: &(f32, usize), b: &(f32, usize)| b.0.total_cmp(&a.0).then(a.1.cmp(&b.1));
                panels.select_nth_unstable_by(SHORTLIST, cmp);
                let mut selected = panels[..SHORTLIST]
                    .iter()
                    .map(|&(_, id)| id)
                    .collect::<Vec<_>>();
                selected.sort_unstable();
                quant::best_q6_panels(weights, &x[i * 1536..(i + 1) * 1536], &selected)
            })
            .collect()
    }
}
fn approximate(g: &Group, lut: &Lut) -> [f32; ROWS] {
    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2") {
        return unsafe { approximate_avx(g, lut) };
    }
    let mut out = [0.; ROWS];
    for b in 0..BLOCKS {
        for r in 0..ROWS {
            let mut s = 0i32;
            for k in 0..32 {
                let v = g.bits[(b * 32 + k) * ROWS + r];
                s += lut.values[b][2 * k][(v & 15) as usize] as i32
                    + lut.values[b][2 * k + 1][(v >> 4) as usize] as i32;
            }
            out[r] += s as f32 * lut.scales[b] * g.scales[b][r];
        }
    }
    out
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn approximate_avx(g: &Group, lut: &Lut) -> [f32; ROWS] {
    use std::arch::x86_64::*;
    unsafe {
        let mask = _mm256_set1_epi8(15);
        let mut sums = [_mm256_setzero_ps(); 4];
        for b in 0..BLOCKS {
            let mut low = _mm256_setzero_si256();
            let mut high = _mm256_setzero_si256();
            for k in (0..32).step_by(2) {
                let calc = |j: usize| {
                    let bits = _mm256_loadu_si256(g.bits.as_ptr().add((b * 32 + j) * ROWS).cast());
                    let lo = _mm256_broadcastsi128_si256(_mm_loadu_si128(
                        lut.values[b][2 * j].as_ptr().cast(),
                    ));
                    let hi = _mm256_broadcastsi128_si256(_mm_loadu_si128(
                        lut.values[b][2 * j + 1].as_ptr().cast(),
                    ));
                    _mm256_add_epi8(
                        _mm256_shuffle_epi8(lo, _mm256_and_si256(bits, mask)),
                        _mm256_shuffle_epi8(
                            hi,
                            _mm256_and_si256(_mm256_srli_epi16::<4>(bits), mask),
                        ),
                    )
                };
                let v = _mm256_add_epi8(calc(k), calc(k + 1));
                low = _mm256_add_epi16(low, _mm256_cvtepi8_epi16(_mm256_castsi256_si128(v)));
                high =
                    _mm256_add_epi16(high, _mm256_cvtepi8_epi16(_mm256_extracti128_si256::<1>(v)));
            }
            let acc = [
                _mm256_cvtepi16_epi32(_mm256_castsi256_si128(low)),
                _mm256_cvtepi16_epi32(_mm256_extracti128_si256::<1>(low)),
                _mm256_cvtepi16_epi32(_mm256_castsi256_si128(high)),
                _mm256_cvtepi16_epi32(_mm256_extracti128_si256::<1>(high)),
            ];
            for j in 0..4 {
                let s = _mm256_mul_ps(
                    _mm256_loadu_ps(g.scales[b].as_ptr().add(j * 8)),
                    _mm256_set1_ps(lut.scales[b]),
                );
                sums[j] = _mm256_add_ps(sums[j], _mm256_mul_ps(_mm256_cvtepi32_ps(acc[j]), s));
            }
        }
        let mut out = [0.; ROWS];
        for j in 0..4 {
            _mm256_storeu_ps(out.as_mut_ptr().add(j * 8), sums[j]);
        }
        out
    }
}
