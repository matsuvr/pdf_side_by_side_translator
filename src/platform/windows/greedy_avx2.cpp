#include <immintrin.h>

// Skip blocks that cannot beat the current maximum. Scan winning blocks in
// token order to preserve the scalar decoder's first-maximum tie break.
int opt_greedy_avx2(const float* logits, int count) {
    int best = 0;
    int i = 0;
    for (; count - i >= 32; i += 32) {
        const __m256 threshold = _mm256_set1_ps(logits[best]);
        __m256 above = _mm256_setzero_ps();
        for (int block = 0; block < 32; block += 8) {
            const __m256 values = _mm256_loadu_ps(logits + i + block);
            above = _mm256_or_ps(above, _mm256_cmp_ps(values, threshold, _CMP_GT_OQ));
        }
        if (_mm256_movemask_ps(above)) {
            for (int j = i; j < i + 32; ++j) {
                if (logits[j] > logits[best]) best = j;
            }
        }
    }
    for (; i < count; ++i) {
        if (logits[i] > logits[best]) best = i;
    }
    return best;
}
