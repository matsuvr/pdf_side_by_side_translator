use super::{H, LayerCache, Q};
use rayon::prelude::*;

const HEADS: usize = Q / H;
const PARALLEL_HEADS_MIN_KEYS: usize = 256;

/// Cache rows in chronological order.
#[derive(Clone, Copy)]
struct Rows<'a> {
    evicted: &'a [f32],
    first: &'a [f32],
    wrapped: &'a [f32],
}

impl<'a> Rows<'a> {
    fn iter(self) -> impl Iterator<Item = &'a [f32]> {
        self.evicted
            .chunks_exact(H)
            .chain(self.first.chunks_exact(H))
            .chain(self.wrapped.chunks_exact(H))
    }
}

#[derive(Clone, Copy)]
struct Window<'a> {
    keys: Rows<'a>,
    values: Rows<'a>,
    len: usize,
}

impl<'a> Window<'a> {
    fn at(cache: &'a LayerCache, pos: usize, evicted: Option<&'a EvictedRows>) -> Self {
        let len = (pos + 1).min(cache.capacity);
        let oldest = pos + 1 - len;
        let evicted_count = evicted.map_or(0, |rows| rows.retained_from.saturating_sub(oldest));
        let start = (oldest + evicted_count) % cache.capacity;
        let first = (len - evicted_count).min(cache.capacity - start);
        let rows = |data: &'a [f32], evicted: &'a [f32]| Rows {
            evicted,
            first: &data[start * H..(start + first) * H],
            wrapped: &data[..(len - evicted_count - first) * H],
        };
        let (old_keys, old_values) = if evicted_count == 0 {
            (&[][..], &[][..])
        } else {
            let old = evicted.unwrap();
            let start = (oldest - old.oldest) * H;
            (&old.keys[start..], &old.values[start..])
        };
        Self {
            keys: rows(&cache.k, old_keys),
            values: rows(&cache.v, old_values),
            len,
        }
    }
}

/// An early query in a batch can still see rows evicted by later appends.
pub(super) struct EvictedRows {
    oldest: usize,
    retained_from: usize,
    keys: Vec<f32>,
    values: Vec<f32>,
}

impl EvictedRows {
    pub(super) fn capture(cache: &LayerCache, positions: &[usize]) -> Self {
        let oldest = (positions[0] + 1).saturating_sub(cache.capacity);
        let retained_from = (positions[positions.len() - 1] + 1).saturating_sub(cache.capacity);
        let len = retained_from - oldest;
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
struct Kernel {
    avx2: bool,
}

impl Kernel {
    fn select() -> Self {
        Self {
            #[cfg(target_arch = "x86_64")]
            avx2: std::is_x86_feature_detected!("avx2"),
            #[cfg(not(target_arch = "x86_64"))]
            avx2: false,
        }
    }

    fn head(self, window: Window<'_>, query: &[f32], out: &mut [f32], scores: &mut [f32]) {
        #[cfg(target_arch = "x86_64")]
        if self.avx2 {
            // The caller provides one complete head and the selected KV window.
            unsafe { head_avx2(window, query, out, scores) };
            return;
        }
        head_scalar(window, query, out, scores);
    }
}

#[inline(always)]
fn score_keys(window: Window<'_>, scores: &mut [f32], dot: impl Fn(&[f32]) -> f32) -> f32 {
    let scale = 1. / (H as f32).sqrt();
    let mut max = f32::NEG_INFINITY;
    for (score, key) in scores.iter_mut().zip(window.keys.iter()) {
        *score = dot(key) * scale;
        max = max.max(*score);
    }
    max
}

fn normalize(scores: &mut [f32], max: f32) {
    let mut sum = 0.;
    for s in scores.iter_mut() {
        *s = (*s - max).exp();
        sum += *s;
    }
    for s in scores {
        *s /= sum;
    }
}

fn head_scalar(window: Window<'_>, query: &[f32], out: &mut [f32], scores: &mut [f32]) {
    let max = score_keys(window, scores, |key| {
        query.iter().zip(key).map(|(a, b)| a * b).sum()
    });
    normalize(scores, max);
    out.fill(0.);
    for (&p, value) in scores.iter().zip(window.values.iter()) {
        for (out, v) in out.iter_mut().zip(value) {
            *out += p * v;
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
#[inline]
unsafe fn dot128_avx(a: &[f32], b: &[f32]) -> f32 {
    use std::arch::x86_64::*;
    unsafe {
        let mut acc = [_mm256_setzero_ps(); 4];
        for i in (0..H).step_by(32) {
            for (j, v) in acc.iter_mut().enumerate() {
                let offset = i + j * 8;
                let x = _mm256_loadu_ps(a.as_ptr().add(offset));
                let y = _mm256_loadu_ps(b.as_ptr().add(offset));
                *v = _mm256_add_ps(*v, _mm256_mul_ps(x, y));
            }
        }
        let v = _mm256_add_ps(_mm256_add_ps(acc[0], acc[1]), _mm256_add_ps(acc[2], acc[3]));
        let mut lanes = [0.; 8];
        _mm256_storeu_ps(lanes.as_mut_ptr(), v);
        lanes.iter().sum()
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn head_avx2(window: Window<'_>, query: &[f32], out: &mut [f32], scores: &mut [f32]) {
    use std::arch::x86_64::*;
    debug_assert_eq!(query.len(), H);
    debug_assert_eq!(out.len(), H);
    unsafe {
        let max = score_keys(window, scores, |key| dot128_avx(query, key));
        normalize(scores, max);
        // Keep half a head in vector registers and preserve the accumulation order.
        for (half, out) in out.chunks_exact_mut(H / 2).enumerate() {
            let mut acc = [_mm256_setzero_ps(); H / 16];
            for (&p, value) in scores.iter().zip(window.values.iter()) {
                let weight = _mm256_set1_ps(p);
                for (i, acc) in acc.iter_mut().enumerate() {
                    let value = _mm256_loadu_ps(value.as_ptr().add(half * (H / 2) + i * 8));
                    *acc = _mm256_add_ps(*acc, _mm256_mul_ps(weight, value));
                }
            }
            for (i, value) in acc.into_iter().enumerate() {
                _mm256_storeu_ps(out.as_mut_ptr().add(i * 8), value);
            }
        }
    }
}

pub(super) fn single(
    query: &[f32],
    cache: &LayerCache,
    pos: usize,
    out: &mut [f32],
    scratch: &mut Vec<f32>,
) {
    let window = Window::at(cache, pos, None);
    let kernel = Kernel::select();
    if window.len >= PARALLEL_HEADS_MIN_KEYS && rayon::current_num_threads() > 1 {
        scratch.resize(window.len * HEADS, 0.);
        query
            .par_chunks_exact(H)
            .zip(out.par_chunks_exact_mut(H))
            .zip(scratch.par_chunks_exact_mut(window.len))
            .with_min_len(2)
            .for_each(|((q, out), scores)| kernel.head(window, q, out, scores));
    } else {
        scratch.resize(window.len, 0.);
        for (q, out) in query.chunks_exact(H).zip(out.chunks_exact_mut(H)) {
            kernel.head(window, q, out, scratch);
        }
    }
}

pub(super) fn prefill(
    qkv: &[f32],
    cache: &LayerCache,
    positions: &[usize],
    evicted: &EvictedRows,
    out: &mut [f32],
    scratch: &mut Vec<f32>,
) {
    let stride = (positions[positions.len() - 1] + 1).min(cache.capacity);
    let kernel = Kernel::select();
    scratch.resize(stride * positions.len(), 0.);
    qkv.par_chunks_exact(Q + 2 * H)
        .zip(out.par_chunks_exact_mut(Q))
        .zip(positions.par_iter().copied())
        .zip(scratch.par_chunks_exact_mut(stride))
        .for_each(|(((row, out), pos), scores)| {
            let window = Window::at(cache, pos, Some(evicted));
            let scores = &mut scores[..window.len];
            for (q, out) in row[..Q].chunks_exact(H).zip(out.chunks_exact_mut(H)) {
                kernel.head(window, q, out, scores);
            }
        });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vector<const N: usize>(seed: usize) -> [f32; N] {
        std::array::from_fn(|i| ((i * 31 + seed * 17) % 127) as f32 / 127. - 0.5)
    }

    fn reference(cache: &LayerCache, query: &[f32], pos: usize) -> Vec<f32> {
        let len = (pos + 1).min(cache.capacity);
        let scale = 1. / (H as f32).sqrt();
        let row = |idx| ((pos + 1 - len + idx) % cache.capacity) * H;
        let mut out = vec![0.; Q];
        for (q, out) in query.chunks_exact(H).zip(out.chunks_exact_mut(H)) {
            let mut scores = vec![0.; len];
            let mut max = f32::NEG_INFINITY;
            for (idx, s) in scores.iter_mut().enumerate() {
                let k = &cache.k[row(idx)..row(idx) + H];
                #[cfg(target_arch = "x86_64")]
                let dot = if std::is_x86_feature_detected!("avx2") {
                    unsafe { dot128_avx(q, k) }
                } else {
                    q.iter().zip(k).map(|(a, b)| a * b).sum()
                };
                #[cfg(not(target_arch = "x86_64"))]
                let dot: f32 = q.iter().zip(k).map(|(a, b)| a * b).sum();
                *s = dot * scale;
                max = max.max(*s);
            }
            let mut sum = 0.;
            for s in &mut scores {
                *s = (*s - max).exp();
                sum += *s;
            }
            for (idx, s) in scores.into_iter().enumerate() {
                for (out, v) in out.iter_mut().zip(&cache.v[row(idx)..row(idx) + H]) {
                    *out += (s / sum) * v;
                }
            }
        }
        out
    }

    #[test]
    fn parallel_heads_preserve_wrapped_window_order() {
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(4)
            .build()
            .unwrap();
        let mut cache = LayerCache::new(273);
        let query = vector::<Q>(123);
        let mut out = vec![0.; Q];
        let mut scratch = Vec::new();
        for i in 0..600 {
            cache.append(i, &vector::<H>(i), &vector::<H>(i + 5));
            if [0, 255, 272, 273, 594].contains(&i) {
                pool.install(|| single(&query, &cache, i, &mut out, &mut scratch));
                assert_eq!(out, reference(&cache, &query, i));
            }
        }
    }

    #[test]
    fn prefill_parallel_rows_keep_causal_prefixes_across_evictions() {
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(4)
            .build()
            .unwrap();
        for first in [64, 250, 535] {
            let mut cache = LayerCache::new(273);
            for i in 0..first {
                cache.append(i, &vector::<H>(i), &vector::<H>(i + 5));
            }
            let positions = (first..first + 32).collect::<Vec<_>>();
            let evicted = EvictedRows::capture(&cache, &positions);
            let mut qkv = Vec::new();
            let mut expected = Vec::new();
            for &i in &positions {
                cache.append(i, &vector::<H>(i), &vector::<H>(i + 5));
                let row = vector::<{ Q + 2 * H }>(i * 5);
                qkv.extend(row);
                expected.extend(reference(&cache, &row[..Q], i));
            }
            let mut out = vec![0.; expected.len()];
            pool.install(|| {
                prefill(
                    &qkv,
                    &cache,
                    &positions,
                    &evicted,
                    &mut out,
                    &mut Vec::new(),
                )
            });
            assert_eq!(out, expected);
        }
    }
}
