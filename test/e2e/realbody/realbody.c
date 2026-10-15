/* One function through the whole pipeline. The immediate 0x28C0DE42 must
 * appear in the Go object's TEXT: the body is this source, compiled. */
#include "goc.h"

void realbody_external_hook(void);

int realbody_add(int x) {
  int v = x + 0x28C0DE42;
  realbody_external_hook(); /* a CALL site: stack map and PCDATA entry */
  return v;
}
