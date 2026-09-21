/* Optional link stubs for goc builtins (not required for color-escape IR pass).
 *
 * uptr MSB: identity stubs here are ONLY for IR/color smoke that never
 * executes encode. Real protocol: link
 *   p19-uptr-runtime/build/libgoc_uptr.a
 * (see p19-uptr-runtime/include/goc_uptr.h). Define GOC_UPTR_REAL and
 * compile against p19 runtime to replace these.
 */
#include "goc.h"
#include <stdlib.h>

#if !defined(GOC_UPTR_REAL)
goc_uptr goc_uptr_from_sptr(goc_sptr p) { return (goc_uptr)p; }
goc_uptr goc_uptr_from_cptr(goc_cptr p) { return (goc_uptr)p; }
goc_sptr goc_uptr_as_sptr(goc_uptr u) { return (goc_sptr)u; }
goc_cptr goc_uptr_as_cptr(goc_uptr u) { return (goc_cptr)u; }
uintptr_t goc_stack_hi(void) { return 0; }
uintptr_t goc_stack_lo(void) { return 0; }
#endif

goc_cptr goc_alloc(size_t n) { return (goc_cptr)malloc(n); }
void goc_free(goc_cptr p) { free((void *)p); }
goc_gptr goc_gptr_from_handle(uintptr_t handle) { return (goc_gptr)handle; }
uintptr_t goc_gptr_to_handle(goc_gptr p) { return (uintptr_t)p; }
bool goc_stack_check(size_t need) { (void)need; return true; }
