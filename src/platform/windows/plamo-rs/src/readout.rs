//! PLaMo output-head specialization: L3-resident 1-bit sign vpshufb LUT scan
//! fused with exact W4 tile rescoring in a single thread-pool dispatch.
use crate::attention::{f16_to_f32_scalar, f32_to_f16_scalar};
use crate::native::{self, Q8};
use crate::pool::Pool;
use crate::weights::{Tensor, Weights};
use std::sync::atomic::{AtomicU64, Ordering};

const GROUPS: usize = 3360;
const ROWS: usize = 32;
const BLOCKS: usize = 6;
const GROUPS_PER_BLOCK: usize = 8; // 256 / 32
const PANEL_BYTES: usize = (BLOCKS * GROUPS_PER_BLOCK) * 160; // 48 * 160 = 7680
const SHORTLIST: usize = 384;

/// Slot mapping so that s_even/s_odd unpack produces rows [0..8], [8..16], [16..24], [24..32]
/// without any lane-crossing extract inside the 16-step inner loop.
const ROW_TO_SLOT: [usize; 32] = [
    0, 2, 4, 6, 8, 10, 12, 14,
    1, 3, 5, 7, 9, 11, 13, 15,
    16, 18, 20, 22, 24, 26, 28, 30,
    17, 19, 21, 23, 25, 27, 29, 31,
];

pub struct Readout {
    bits: Vec<u8>,     // GROUPS * BLOCKS * 1024
    scales: Vec<u16>,  // GROUPS * BLOCKS * 32 (IEEE-754 F16)
}

#[repr(C, align(64))]
struct Lut {
    values: [[[i8; 16]; 64]; BLOCKS],
    scales: [f32; BLOCKS],
}

impl Lut {
    fn new(x: &[f32]) -> Self {
        let mut r = Self {
            values: [[[0; 16]; 64]; BLOCKS],
            scales: [0.0; BLOCKS],
        };
        for b in 0..BLOCKS {
            let xb = &x[b * 256..(b + 1) * 256];
            let mut max_g4 = 0.0f32;
            for j in 0..64 {
                let s = xb[j * 4].abs()
                    + xb[j * 4 + 1].abs()
                    + xb[j * 4 + 2].abs()
                    + xb[j * 4 + 3].abs();
                if s > max_g4 {
                    max_g4 = s;
                }
            }
            r.scales[b] = max_g4 * (1.0 / 31.0);
            let inv = if max_g4 > 0.0 { 31.0 / max_g4 } else { 0.0 };
            for j in 0..64 {
                let v0 = xb[j * 4] * inv;
                let v1 = xb[j * 4 + 1] * inv;
                let v2 = xb[j * 4 + 2] * inv;
                let v3 = xb[j * 4 + 3] * inv;
                let dst = &mut r.values[b][j];
                // Exploit mask ^ 15 negation symmetry to halve LUT construction.
                for m in 0..8 {
                    let val = if m & 1 != 0 { v0 } else { -v0 }
                        + if m & 2 != 0 { v1 } else { -v1 }
                        + if m & 4 != 0 { v2 } else { -v2 }
                        - v3;
                    let q = val.round_ties_even().clamp(-31.0, 31.0) as i8;
                    dst[m] = q;
                    dst[m ^ 15] = -q;
                }
            }
        }
        r
    }
}

impl Readout {
    pub(crate) fn build_native(pool: &Pool, g: &Weights, t: &Tensor) -> Option<Self> {
        #[cfg(target_arch = "x86_64")]
        if std::is_x86_feature_detected!("avx2") {
            let mut bits = vec![0u8; GROUPS * BLOCKS * 1024];
            let mut scales = vec![0u16; GROUPS * BLOCKS * ROWS];
            let raw = g.data(t);
            let bits_addr = bits.as_mut_ptr() as usize;
            let scales_addr = scales.as_mut_ptr() as usize;
            let nth = pool.num_threads().min(GROUPS).max(1);

            pool.dispatch(nth, |ith, nth| {
                let g_start = ith * GROUPS / nth;
                let g_end = (ith + 1) * GROUPS / nth;
                let bits_ptr = bits_addr as *mut u8;
                let scales_ptr = scales_addr as *mut u16;
                let mut w_blk = [0.0f32; 256];

                for grp in g_start..g_end {
                    let g_bits = unsafe { bits_ptr.add(grp * BLOCKS * 1024) };
                    let g_scales = unsafe { scales_ptr.add(grp * BLOCKS * ROWS) };
                    // Each 32-row group is 4 consecutive 8-row TILE panels.
                    for panel_local in 0..4 {
                        let panel = &raw[(grp * 4 + panel_local) * PANEL_BYTES
                            ..(grp * 4 + panel_local + 1) * PANEL_BYTES];
                        for r_local in 0..8 {
                            let row = panel_local * 8 + r_local;
                            let slot = ROW_TO_SLOT[row];
                            for b in 0..BLOCKS {
                                let mut abs_sum = 0.0f32;
                                // 8 groups of 32 weights per 256-weight superblock.
                                for sg in 0..GROUPS_PER_BLOCK {
                                    let off = (b * GROUPS_PER_BLOCK + sg) * 160;
                                    let blk = &panel[off..off + 160];
                                    let d = f32::from_le_bytes(
                                        blk[r_local * 4..r_local * 4 + 4].try_into().unwrap(),
                                    );
                                    let base = sg * 32;
                                    for k in 0..16 {
                                        let byte = blk[(k / 4 + 1) * 32 + r_local * 4 + k % 4];
                                        let v_lo = d * ((byte & 15) as f32 - 8.0);
                                        let v_hi = d * ((byte >> 4) as f32 - 8.0);
                                        w_blk[base + k] = v_lo;
                                        w_blk[base + 16 + k] = v_hi;
                                        abs_sum += v_lo.abs() + v_hi.abs();
                                    }
                                }
                                unsafe {
                                    *g_scales.add(b * ROWS + row) =
                                        f32_to_f16_scalar(abs_sum * (1.0 / 256.0));
                                }
                                let b_bits = unsafe { g_bits.add(b * 1024) };
                                for k in 0..32 {
                                    let wk = &w_blk[k * 8..k * 8 + 8];
                                    let mut mask = 0u8;
                                    if wk[0] >= 0.0 { mask |= 0x01; }
                                    if wk[1] >= 0.0 { mask |= 0x02; }
                                    if wk[2] >= 0.0 { mask |= 0x04; }
                                    if wk[3] >= 0.0 { mask |= 0x08; }
                                    if wk[4] >= 0.0 { mask |= 0x10; }
                                    if wk[5] >= 0.0 { mask |= 0x20; }
                                    if wk[6] >= 0.0 { mask |= 0x40; }
                                    if wk[7] >= 0.0 { mask |= 0x80; }
                                    unsafe {
                                        *b_bits.add(k * 32 + slot) = mask;
                                    }
                                }
                            }
                        }
                    }
                }
            });

            return Some(Self { bits, scales });
        }
        let _ = (pool, g, t);
        None
    }

    pub(crate) fn best_native(&self, pool: &Pool, g: &Weights, t: &Tensor, x: &[f32]) -> u32 {
        let lut = Lut::new(x);
        let mut q = [Q8::default(); 48];
        native::quantize_q8_slice(x, &mut q);
        let bytes = g.data(t);
        let head_isa = native::select_head_isa();

        #[cfg(target_arch = "x86_64")]
        let use_f16c_fma = std::is_x86_feature_detected!("avx2")
            && std::is_x86_feature_detected!("fma")
            && std::is_x86_feature_detected!("f16c");
        #[cfg(not(target_arch = "x86_64"))]
        let use_f16c_fma = false;

        let nth = pool.decode_threads().min(GROUPS).min(256).max(1);
        let winner_scores: [AtomicU64; 256] =
            std::array::from_fn(|_| AtomicU64::new(f32::NEG_INFINITY.to_bits() as u64));
        let winner_ids: [AtomicU64; 256] =
            std::array::from_fn(|_| AtomicU64::new(u32::MAX as u64));

        pool.dispatch(nth, |ith, nth| {
            let k_panels = ((SHORTLIST + nth - 1) / nth).clamp(20, 128);

            let (best_score, best_id) = {
                #[cfg(target_arch = "x86_64")]
                if use_f16c_fma {
                    unsafe {
                        eval_chunk_avx2_fma_f16c(
                            &self.bits,
                            &self.scales,
                            &lut,
                            bytes,
                            &q,
                            head_isa,
                            ith,
                            nth,
                            k_panels,
                        )
                    }
                } else {
                    eval_chunk_fallback(
                        &self.bits,
                        &self.scales,
                        &lut,
                        bytes,
                        &q,
                        head_isa,
                        ith,
                        nth,
                        k_panels,
                    )
                }
                #[cfg(not(target_arch = "x86_64"))]
                eval_chunk_fallback(
                    &self.bits,
                    &self.scales,
                    &lut,
                    bytes,
                    &q,
                    head_isa,
                    ith,
                    nth,
                    k_panels,
                )
            };

            winner_scores[ith].store(best_score.to_bits() as u64, Ordering::Relaxed);
            winner_ids[ith].store(best_id as u64, Ordering::Relaxed);
        });

        let mut best_score = f32::NEG_INFINITY;
        let mut best_id = u32::MAX;
        for ith in 0..nth {
            let s = f32::from_bits(winner_scores[ith].load(Ordering::Relaxed) as u32);
            let id = winner_ids[ith].load(Ordering::Relaxed) as u32;
            if s > best_score || (s == best_score && id < best_id) {
                best_score = s;
                best_id = id;
            }
        }
        best_id
    }
}

const STRIPE_GROUPS: usize = 4; // 4 groups = 128 tokens = 24 KB contiguous per stripe

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma,f16c")]
unsafe fn eval_chunk_avx2_fma_f16c(
    bits: &[u8],
    scales: &[u16],
    lut: &Lut,
    w4_bytes: &[u8],
    q: &[Q8; 48],
    head_isa: native::HeadIsa,
    ith: usize,
    nth: usize,
    k_panels: usize,
) -> (f32, u32) {
    use std::arch::x86_64::*;

    let mut top_score = [-1e30f32; 128];
    let mut top_panel = [usize::MAX; 128];
    let mut thresh = -1e30f32;
    let mut min_idx = 0usize;

    unsafe {
        let m0f = _mm256_set1_epi8(0x0F);
        let mut v_thresh = _mm256_set1_ps(thresh);

        #[inline(always)]
        unsafe fn hmax8(v: __m256) -> f32 {
            unsafe {
                let lo = _mm256_castps256_ps128(v);
                let hi = _mm256_extractf128_ps::<1>(v);
                let m = _mm_max_ps(lo, hi);
                let m = _mm_max_ps(m, _mm_movehl_ps(m, m));
                let m = _mm_max_ss(m, _mm_shuffle_ps::<1>(m, m));
                _mm_cvtss_f32(m)
            }
        }

        let mut consider = |acc: __m256, panel_id: usize| {
            if _mm256_movemask_ps(_mm256_cmp_ps::<_CMP_GT_OQ>(acc, v_thresh)) != 0 {
                let pm = hmax8(acc);
                if pm > thresh {
                    top_score[min_idx] = pm;
                    top_panel[min_idx] = panel_id;
                    let mut new_min = top_score[0];
                    let mut new_idx = 0;
                    for i in 1..k_panels {
                        if top_score[i] < new_min {
                            new_min = top_score[i];
                            new_idx = i;
                        }
                    }
                    thresh = new_min;
                    min_idx = new_idx;
                    v_thresh = _mm256_set1_ps(thresh);
                }
            }
        };

        let n_stripes = (GROUPS + STRIPE_GROUPS - 1) / STRIPE_GROUPS;
        let mut stripe = ith;
        while stripe < n_stripes {
            let g_start = stripe * STRIPE_GROUPS;
            let g_end = (g_start + STRIPE_GROUPS).min(GROUPS);
            for g in g_start..g_end {
                let g_bits = bits.as_ptr().add(g * BLOCKS * 1024);
                let g_scales = scales.as_ptr().add(g * BLOCKS * ROWS);

                if g + 1 < g_end {
                    _mm_prefetch::<_MM_HINT_T0>(g_bits.add(BLOCKS * 1024).cast());
                } else if stripe + nth < n_stripes {
                    let next_g = (stripe + nth) * STRIPE_GROUPS;
                    _mm_prefetch::<_MM_HINT_T0>(bits.as_ptr().add(next_g * BLOCKS * 1024).cast());
                }

                let mut acc0 = _mm256_setzero_ps();
                let mut acc1 = _mm256_setzero_ps();
                let mut acc2 = _mm256_setzero_ps();
                let mut acc3 = _mm256_setzero_ps();

                for b in 0..BLOCKS {
                    let b_bits = g_bits.add(b * 1024);
                    let b_lut = &lut.values[b];

                    let mut s_even = _mm256_setzero_si256();
                    let mut s_odd = _mm256_setzero_si256();

                    for k in (0..32).step_by(2) {
                        let b0 = _mm256_loadu_si256(b_bits.add(k * 32).cast());
                        let b1 = _mm256_loadu_si256(b_bits.add((k + 1) * 32).cast());

                        let l0 = _mm256_broadcastsi128_si256(_mm_loadu_si128(
                            b_lut[k * 2].as_ptr().cast(),
                        ));
                        let l1 = _mm256_broadcastsi128_si256(_mm_loadu_si128(
                            b_lut[k * 2 + 1].as_ptr().cast(),
                        ));
                        let l2 = _mm256_broadcastsi128_si256(_mm_loadu_si128(
                            b_lut[k * 2 + 2].as_ptr().cast(),
                        ));
                        let l3 = _mm256_broadcastsi128_si256(_mm_loadu_si128(
                            b_lut[k * 2 + 3].as_ptr().cast(),
                        ));

                        let v0 = _mm256_shuffle_epi8(l0, _mm256_and_si256(b0, m0f));
                        let v1 = _mm256_shuffle_epi8(
                            l1,
                            _mm256_and_si256(_mm256_srli_epi16::<4>(b0), m0f),
                        );
                        let v2 = _mm256_shuffle_epi8(l2, _mm256_and_si256(b1, m0f));
                        let v3 = _mm256_shuffle_epi8(
                            l3,
                            _mm256_and_si256(_mm256_srli_epi16::<4>(b1), m0f),
                        );

                        let s8 = _mm256_add_epi8(_mm256_add_epi8(v0, v1), _mm256_add_epi8(v2, v3));
                        s_even = _mm256_add_epi16(s_even, _mm256_srai_epi16::<8>(_mm256_slli_epi16::<8>(s8)));
                        s_odd = _mm256_add_epi16(s_odd, _mm256_srai_epi16::<8>(s8));
                    }

                    let xs = _mm256_set1_ps(lut.scales[b]);
                    let sc_b = g_scales.add(b * ROWS);
                    let sc0 = _mm256_mul_ps(_mm256_cvtph_ps(_mm_loadu_si128(sc_b.cast())), xs);
                    let sc1 = _mm256_mul_ps(_mm256_cvtph_ps(_mm_loadu_si128(sc_b.add(8).cast())), xs);
                    let sc2 = _mm256_mul_ps(_mm256_cvtph_ps(_mm_loadu_si128(sc_b.add(16).cast())), xs);
                    let sc3 = _mm256_mul_ps(_mm256_cvtph_ps(_mm_loadu_si128(sc_b.add(24).cast())), xs);

                    acc0 = _mm256_fmadd_ps(
                        _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(_mm256_castsi256_si128(s_even))),
                        sc0,
                        acc0,
                    );
                    acc1 = _mm256_fmadd_ps(
                        _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(_mm256_castsi256_si128(s_odd))),
                        sc1,
                        acc1,
                    );
                    acc2 = _mm256_fmadd_ps(
                        _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(_mm256_extracti128_si256::<1>(s_even))),
                        sc2,
                        acc2,
                    );
                    acc3 = _mm256_fmadd_ps(
                        _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(_mm256_extracti128_si256::<1>(s_odd))),
                        sc3,
                        acc3,
                    );
                }

                consider(acc0, g * 4);
                consider(acc1, g * 4 + 1);
                consider(acc2, g * 4 + 2);
                consider(acc3, g * 4 + 3);
            }
            stripe += nth;
        }

        // Stage 2: Exact W4 rescoring of shortlisted panels in pairs (zero barrier!).
        let mut valid_panels = [0usize; 128];
        let mut n_valid = 0usize;
        for i in 0..k_panels {
            if top_panel[i] != usize::MAX {
                valid_panels[n_valid] = top_panel[i];
                n_valid += 1;
            }
        }
        let mut best_score = f32::NEG_INFINITY;
        let mut best_id = u32::MAX;
        let mut d0 = [0.0f32; 8];
        let mut d1 = [0.0f32; 8];
        let mut i = 0;
        while i + 1 < n_valid {
            let p0 = valid_panels[i];
            let p1 = valid_panels[i + 1];
            if i + 2 < n_valid {
                _mm_prefetch::<_MM_HINT_T0>(
                    w4_bytes.as_ptr().add(valid_panels[i + 2] * PANEL_BYTES).cast(),
                );
            }
            if i + 3 < n_valid {
                _mm_prefetch::<_MM_HINT_T0>(
                    w4_bytes.as_ptr().add(valid_panels[i + 3] * PANEL_BYTES).cast(),
                );
            }
            let t0 = &w4_bytes[p0 * PANEL_BYTES..(p0 + 1) * PANEL_BYTES];
            let t1 = &w4_bytes[p1 * PANEL_BYTES..(p1 + 1) * PANEL_BYTES];
            native::score_2panels_w4(head_isa, t0, t1, q, &mut d0, &mut d1);
            for (r, &s) in d0.iter().enumerate() {
                let id = (p0 * 8 + r) as u32;
                if s > best_score || (s == best_score && id < best_id) {
                    best_score = s;
                    best_id = id;
                }
            }
            for (r, &s) in d1.iter().enumerate() {
                let id = (p1 * 8 + r) as u32;
                if s > best_score || (s == best_score && id < best_id) {
                    best_score = s;
                    best_id = id;
                }
            }
            i += 2;
        }
        if i < n_valid {
            let p0 = valid_panels[i];
            let t0 = &w4_bytes[p0 * PANEL_BYTES..(p0 + 1) * PANEL_BYTES];
            native::score_panel_w4(head_isa, t0, q, &mut d0);
            for (r, &s) in d0.iter().enumerate() {
                let id = (p0 * 8 + r) as u32;
                if s > best_score || (s == best_score && id < best_id) {
                    best_score = s;
                    best_id = id;
                }
            }
        }
        (best_score, best_id)
    }
}

fn eval_chunk_fallback(
    bits: &[u8],
    scales: &[u16],
    lut: &Lut,
    w4_bytes: &[u8],
    q: &[Q8; 48],
    head_isa: native::HeadIsa,
    ith: usize,
    nth: usize,
    k_panels: usize,
) -> (f32, u32) {
    let mut top_score = [-1e30f32; 128];
    let mut top_panel = [usize::MAX; 128];
    let mut thresh = -1e30f32;
    let mut min_idx = 0usize;

    let n_stripes = (GROUPS + STRIPE_GROUPS - 1) / STRIPE_GROUPS;
    let mut stripe = ith;
    while stripe < n_stripes {
        let g_start = stripe * STRIPE_GROUPS;
        let g_end = (g_start + STRIPE_GROUPS).min(GROUPS);
        for g in g_start..g_end {
            let g_bits = &bits[g * BLOCKS * 1024..(g + 1) * BLOCKS * 1024];
            let g_scales = &scales[g * BLOCKS * ROWS..(g + 1) * BLOCKS * ROWS];
            let mut out = [0.0f32; ROWS];
            for b in 0..BLOCKS {
                let b_bits = &g_bits[b * 1024..(b + 1) * 1024];
                let b_scales = &g_scales[b * ROWS..(b + 1) * ROWS];
                let xs = lut.scales[b];
                for r in 0..ROWS {
                    let slot = ROW_TO_SLOT[r];
                    let mut s = 0i32;
                    for k in 0..32 {
                        let v = b_bits[k * 32 + slot];
                        s += lut.values[b][2 * k][(v & 15) as usize] as i32
                            + lut.values[b][2 * k + 1][(v >> 4) as usize] as i32;
                    }
                    out[r] += s as f32 * xs * f16_to_f32_scalar(b_scales[r]);
                }
            }
            for p in 0..4 {
                let pm = out[p * 8..p * 8 + 8]
                    .iter()
                    .copied()
                    .fold(f32::NEG_INFINITY, f32::max);
                if pm > thresh {
                    top_score[min_idx] = pm;
                    top_panel[min_idx] = g * 4 + p;
                    let mut new_min = top_score[0];
                    let mut new_idx = 0;
                    for i in 1..k_panels {
                        if top_score[i] < new_min {
                            new_min = top_score[i];
                            new_idx = i;
                        }
                    }
                    thresh = new_min;
                    min_idx = new_idx;
                }
            }
        }
        stripe += nth;
    }

    let mut best_score = f32::NEG_INFINITY;
    let mut best_id = u32::MAX;
    let mut dst = [0.0f32; 8];
    for i in 0..k_panels {
        let panel = top_panel[i];
        if panel == usize::MAX {
            continue;
        }
        let tile = &w4_bytes[panel * PANEL_BYTES..(panel + 1) * PANEL_BYTES];
        native::score_panel_w4(head_isa, tile, q, &mut dst);
        for (r, &s) in dst.iter().enumerate() {
            let id = (panel * 8 + r) as u32;
            if s > best_score || (s == best_score && id < best_id) {
                best_score = s;
                best_id = id;
            }
        }
    }
    (best_score, best_id)
}
