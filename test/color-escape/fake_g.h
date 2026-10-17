/* Host stand-in for the goroutine g that goc's uptr helpers read.
 *
 * goc-inline-gate gives goc_stack_lo/hi, goc_uptr_* and friends inline
 * bodies that load g from %fs:-8 and stack.lo/hi from g+0/g+8, exactly as in
 * a Go binary. In a host executable whose only TLS variable is this
 * local-exec pointer, that variable is the word at %fs:-8 (x86-64 TLS
 * variant II: the executable's block ends at the thread pointer). Pointing it
 * at a fake g lets a host test move stack.lo/hi and run the compiler's own
 * helper bodies, with no runtime library linked.
 */
#ifndef GOC_TEST_FAKE_G_H
#define GOC_TEST_FAKE_G_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(__x86_64__)
#error "fake_g.h models the linux/amd64 g read (%fs:-8)"
#endif

struct goc_fake_g {
  uintptr_t stack_lo; /* g+0 */
  uintptr_t stack_hi; /* g+8 */
};

static struct goc_fake_g goc_fake_g_word;
static __thread struct goc_fake_g *goc_fake_g_tls
    __attribute__((tls_model("local-exec")));

static void goc_fake_g_set(uintptr_t lo, uintptr_t hi) {
  uintptr_t tp;
  __asm__("movq %%fs:0, %0" : "=r"(tp));
  if ((uintptr_t)&goc_fake_g_tls != tp - 8) {
    fprintf(stderr, "fake_g: TLS slot %p is not %%fs:-8 (%#lx)\n",
            (void *)&goc_fake_g_tls, (unsigned long)(tp - 8));
    abort();
  }
  goc_fake_g_word.stack_lo = lo;
  goc_fake_g_word.stack_hi = hi;
  goc_fake_g_tls = &goc_fake_g_word;
}

#endif /* GOC_TEST_FAKE_G_H */
