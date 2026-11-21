"""CPU checks for the two less-obvious fused-kernel algorithms.

Run with ``python tests/test_kernel_math.py`` (NumPy required). These compare
the packed-affine dot and online/grouped softmax with independently expressed
dense references. They check the algorithms, not compiled Metal execution;
the CLI's --verify command compares actual Metal and MLX model logits on a Mac.
"""

import unittest

import numpy as np


def packed_dot(packed, scales, biases, activations):
    """The mask/prescaled-input method used by the Metal row kernel."""
    cols = activations.size
    lanes = np.zeros(32, dtype=np.float32)
    for block in range(0, cols, 512):
        for lane in range(32):
            start = block + lane * 16
            source = activations[start : start + 16]
            scaled = source / np.array([1, 16, 256, 4096] * 4, dtype=np.float32)
            words = packed[start // 8 : start // 8 + 2]
            masked = np.array(
                [
                    (int(words[i // 8]) >> (16 * ((i % 8) // 4)))
                    & (15 << (4 * (i % 4)))
                    for i in range(16)
                ],
                dtype=np.float32,
            )
            group = start // 64
            contribution = (
                np.float32(scales[group]) * np.sum(scaled * masked, dtype=np.float32)
                + np.float32(biases[group]) * np.sum(source, dtype=np.float32)
            )
            lanes[lane] += contribution
    return np.sum(lanes, dtype=np.float32)


def dense_affine_reference(packed, scales, biases, activations):
    """Unpack each nibble normally, reconstruct all weights, then take a dot."""
    shifts = np.arange(8, dtype=np.uint32) * 4
    integers = ((packed[:, None] >> shifts) & np.uint32(15)).reshape(-1)
    weights = (
        integers.astype(np.float64) * np.repeat(scales.astype(np.float64), 64)
        + np.repeat(biases.astype(np.float64), 64)
    )
    return weights @ activations.astype(np.float64)


def online_grouped_attention(scores, values, groups=32):
    """Online FP32 softmax per SIMD group, followed by max-rescaled merging.

    The last entry is the current token, which group 0 always includes. The
    other groups may have no history, as occurs at the beginning of generation.
    """
    history = len(scores) - 1
    partial = []
    for group in range(groups):
        peak = scores[-1] if group == 0 else np.float32(-np.inf)
        denominator = np.float32(1 if group == 0 else 0)
        numerator = values[-1].copy() if group == 0 else np.zeros(values.shape[1], np.float32)
        for i in range(group, history, groups):
            next_peak = max(peak, scores[i])
            old_scale = np.exp(peak - next_peak) if denominator > 0 else np.float32(0)
            new_scale = np.exp(scores[i] - next_peak)
            denominator = denominator * old_scale + new_scale
            numerator = numerator * old_scale + values[i] * new_scale
            peak = next_peak
        partial.append((peak, denominator, numerator))
    peak = max(item[0] for item in partial)
    denominator = np.float32(0)
    numerator = np.zeros(values.shape[1], np.float32)
    for local_peak, local_denominator, local_numerator in partial:
        if local_denominator > 0:
            factor = np.exp(local_peak - peak)
            denominator += local_denominator * factor
            numerator += local_numerator * factor
    return numerator / denominator


def dense_attention_reference(scores, values):
    """Ordinary stabilized FP64 softmax and a dense weighted sum."""
    logits = scores.astype(np.float64)
    probabilities = np.exp(logits - logits.max())
    probabilities /= probabilities.sum()
    return probabilities @ values.astype(np.float64)


class KernelMathTests(unittest.TestCase):
    def test_packed_affine_matches_dequantized_dot(self):
        rng = np.random.default_rng(20261121)
        # All input widths used by the official 0.8B decoder and output head.
        for cols in (1536, 2048, 6144):
            for _ in range(16):
                with self.subTest(cols=cols):
                    integers = rng.integers(0, 16, size=cols, dtype=np.uint32)
                    words = np.sum(
                        integers.reshape(-1, 8) << (4 * np.arange(8, dtype=np.uint32)),
                        axis=1,
                        dtype=np.uint32,
                    )
                    scales = rng.uniform(0.001, 0.2, cols // 64).astype(np.float16)
                    biases = rng.uniform(-0.9, 0.9, cols // 64).astype(np.float16)
                    x = rng.normal(size=cols).astype(np.float32)
                    np.testing.assert_allclose(
                        packed_dot(words, scales, biases, x),
                        dense_affine_reference(words, scales, biases, x),
                        rtol=3e-5,
                        atol=3e-5,
                    )

    def test_online_softmax_empty_partial_groups_and_long_history(self):
        rng = np.random.default_rng(17)
        for history in (0, 1, 31, 32, 33, 2047, 2048, 32768):
            with self.subTest(history=history):
                scores = (rng.normal(size=history + 1) * 75).astype(np.float32)
                values = rng.normal(size=(history + 1, 128)).astype(np.float32)
                actual = online_grouped_attention(scores, values)
                self.assertTrue(np.isfinite(actual).all())
                np.testing.assert_allclose(
                    actual, dense_attention_reference(scores, values), rtol=2e-5, atol=2e-6
                )

    def test_softmax_handles_history_far_above_current_token(self):
        # An own-token-relative exponential with a fixed cap would distort this.
        rng = np.random.default_rng(22)
        scores = np.array([1000, 999, -1000], dtype=np.float32)
        values = rng.normal(size=(3, 128)).astype(np.float32)
        actual = online_grouped_attention(scores, values)
        self.assertTrue(np.isfinite(actual).all())
        np.testing.assert_allclose(
            actual, dense_attention_reference(scores, values), rtol=2e-5, atol=2e-6
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
