#include <immintrin.h>
__m128 goc_sse_add(__m128 a, __m128 b) { return _mm_add_ps(a, b); }
