// SPDX-License-Identifier: Apache-2.0
// Stand-in for wmtr's winemetal.so in transport_test: answers SVR_HELLO like the host's VR exporter.
#include <string.h>

#include "steamac_vr_guest.h"

__attribute__((visibility("default"))) int wmtr_vr_call(const void *req, unsigned req_size, void *rep, unsigned rep_cap) {
  const struct svr_hdr *h = req;
  struct svr_hello_rep r;
  if (req_size < sizeof(*h) || h->op != SVR_HELLO || rep_cap < sizeof(r)) return -22;
  memset(&r, 0, sizeof(r));
  r.h.op = SVR_HELLO;
  r.h.size = sizeof(r);
  r.version = SVR_VERSION;
  memcpy(rep, &r, sizeof(r));
  return 0;
}

static unsigned fake_call(void *args) { (void)args; return 0; }
__attribute__((visibility("default"))) const void *__wine_unix_call_funcs[256] = {[0 ... 255] = (const void *)fake_call};
