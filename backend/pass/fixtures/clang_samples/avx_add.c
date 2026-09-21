#include <immintrin.h>
__m256 goc_avx_add(__m256 a, __m256 b) { return _mm256_add_ps(a, b); }
