/* libFuzzer target: the host executor's request decoding (wmtr_host.m execute/reply: record framing, relocations,
 * WMTR_REF_EXTRA/ARENA references, output ranges, reply slot, control requests incl. WMTR_CTL_VR_QUERY) with a
 * request the guest wrote itself. The implementations are stubs that read and write their whole params struct;
 * calls that dereference guest handles by design (raw Metal object pointers: the trust boundary of
 * fxgl/steamac#14) are mapped to a plain call, so a report here is a decoding bug, not the handle model.
 * Build (Homebrew LLVM; Apple clang has no libFuzzer), from wmtr/:
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -ObjC -Iinclude -Isrc -I../host/vrhost/include \
 *     -I../guest/vr/include -I$DXMT/src/winemetal -I$DXMT/src/airconv tests/fuzz_wmtr_host.m src/wmtr_host.m \
 *     [src/wmtr_vr.m] -framework Foundation -framework Metal -framework Cocoa -framework QuartzCore \
 *     -framework IOSurface -o fuzz_wmtr_host
 * (Homebrew LLVM 23 with lld: add -fno-objc-msgsend-class-selector-stubs.)
 * Run with ASAN_OPTIONS=allow_user_segv_handler=0 (the executor's fault containment would hide ASan's reports). */
#import <Foundation/Foundation.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

#define WINEMETAL_API
#include "winemetal_thunks.h"
#include "airconv_thunks.h"
#include "wmtr_wire.h"
#include "wmtr_host.h"

typedef int NTSTATUS;
#ifndef WMTR_NCALLS
#define WMTR_NCALLS 146 /* DXMT's table only (no WMTVRSubmit) */
#endif
#ifndef WMTR_F_CLIENT_HANDLE
#define WMTR_F_CLIENT_HANDLE 0
#endif

/* stubs: read every byte of the call's params struct, then write all of it (results; object results 0, as the
 * host keeps returned objects alive) */
static volatile uint8_t sink;
#define X(i, n, t, c)                                                                                                \
  static NTSTATUS stub_##i(void *p) {                                                                                \
    uint8_t *b = p, x = 0;                                                                                            \
    for (size_t k = 0; k < sizeof(t); k++) x ^= b[k];                                                                 \
    sink = x;                                                                                                         \
    memset(b, 0, sizeof(t));                                                                                          \
    return 0;                                                                                                         \
  }
#include "wmtr_calls.inc"
#undef X
const void *wmtr_impl_funcs[WMTR_NCALLS] = {
#define X(i, n, t, c) [i] = stub_##i,
#include "wmtr_calls.inc"
#undef X
};

static void *blob_base[2];
static uint64_t blob_size[2];
static void *f_blob_ptr(void *c, uint32_t b, uint64_t *s) {
  if (b >= 2 || !blob_base[b]) return NULL;
  *s = blob_size[b];
  return blob_base[b];
}
static void f_wake(void *c, uint32_t s) {}
static const struct wmtr_host_ops ops = {f_blob_ptr, f_wake, NULL, NULL, NULL};
static struct wmtr_host *H;
static struct wmtr_ring *R;

/* calls the host itself handles with guest handles (pools, views, events, buffers, VR, paths) */
static int handle_call(uint32_t c) {
  switch (c) {
  case 0: case 1: case 10: case 17: case 18: case 47: case 48: case 72: case 73: case 104: case 108: case 110: case 146:
  case 112: case 113: case 115: case 117: case 119: return 1;
  default: return 0;
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (!H) {
    void *ring;
    posix_memalign(&ring, WMTR_PAGE, WMTR_RING_BLOB_BYTES);
    memset(ring, 0, WMTR_RING_BLOB_BYTES);
    R = ring;
    R->magic = WMTR_RING_MAGIC; R->version = WMTR_VERSION;
    blob_base[0] = ring; blob_size[0] = WMTR_RING_BLOB_BYTES;
    blob_size[1] = 1 << 20;
    posix_memalign(&blob_base[1], WMTR_PAGE, blob_size[1]);
    memset(blob_base[1], 0, blob_size[1]);
    H = wmtr_host_create(ring, &ops, NULL);
    wmtr_host_set_spin(H, 0);
  }
  /* input: call, flags, params_size, n_reloc, n_out, payload_size, then the record body as is */
  struct { uint16_t call, flags; uint32_t params_size; uint16_t n_reloc, n_out; uint32_t payload_size; } in;
  if (size < sizeof in || size > 256 * 1024) return 0;
  memcpy(&in, data, sizeof in);
  data += sizeof in; size -= sizeof in;
  struct wmtr_req_ext hdr = {0};
  hdr.h.flags = WMTR_F_SYNC | (in.flags & (WMTR_F_CONTROL | WMTR_F_CLIENT_HANDLE));
  hdr.h.call = hdr.h.flags & WMTR_F_CONTROL ? in.call % 6 : in.call % (WMTR_NCALLS + 2);
  if (!(hdr.h.flags & WMTR_F_CONTROL) && hdr.h.call < WMTR_NCALLS && handle_call(hdr.h.call)) hdr.h.call = 39;
  hdr.h.slot = 0;
  hdr.h.params_size = in.params_size;
  hdr.n_reloc = in.n_reloc; hdr.n_out = in.n_out;
  hdr.payload_size = in.payload_size;
  uint32_t total = WMTR_ALIGN8((uint32_t)(sizeof hdr + size));
  hdr.h.size = total;
  struct wmtr_slot *slot = (struct wmtr_slot *)((uint8_t *)R + WMTR_SLOTS_OFFSET);
  hdr.h.seq = slot->done_seq + 1;
  uint8_t *ring_data = (uint8_t *)R + WMTR_RING_DATA_OFFSET;
  uint64_t head = R->head;
  uint32_t pos = (uint32_t)(head % WMTR_RING_BYTES), to_end = WMTR_RING_BYTES - pos;
  if (to_end < total) {
    struct wmtr_req w = {0}; w.size = to_end; w.flags = WMTR_F_WRAP;
    if (to_end >= sizeof w) memcpy(ring_data + pos, &w, sizeof w);
    head += to_end; pos = 0;
  }
  memset(ring_data + pos, 0, total);
  memcpy(ring_data + pos, &hdr, sizeof hdr);
  memcpy(ring_data + pos + sizeof hdr, data, size);
  __atomic_store_n(&R->head, head + total, __ATOMIC_RELEASE);
  wmtr_host_kick(H);
  while (__atomic_load_n(&slot->done_seq, __ATOMIC_ACQUIRE) < hdr.h.seq) sched_yield();
  return 0;
}
