/* libFuzzer target: the host executor's request decoding (wmtr_host.m execute/reply: record framing, relocations,
 * WMTR_REF_EXTRA/ARENA references, output ranges, reply slot, control requests incl. WMTR_CTL_VR_QUERY) and its
 * object handles (wmtr_handles.h: every handle and pointer field of every call, command lists, handle arrays,
 * retain/release counting) with a request the guest wrote itself. The implementations are stubs that read and
 * write their whole params struct and return real objects in their result fields; the calls the executor
 * implements itself (pools, buffers, events, listeners, views, VR, paths) run for real on the objects the
 * handles name. The context's table is seeded with real Metal objects of several kinds; a params word tagged
 * "WO" (0x574F) whose bit 40 is clear is replaced by one of those valid handles (low byte picks it), so the fuzzer
 * reaches both valid handles of every kind in every field and forged / stale / wrong-kind ones.
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
#include "wmtr_handles.h"
#include <Metal/Metal.h>
extern uint64_t wmtr_host_debug_handle(struct wmtr_host *h, uint64_t obj, int kind, int plus1);
#define WMTR_NCALLS_REAL WMTR_NCALLS
#include "wmtr_handles.inc"

typedef int NTSTATUS;
#ifndef WMTR_NCALLS
#define WMTR_NCALLS 146 /* DXMT's table only (no WMTVRSubmit) */
#endif
#ifndef WMTR_F_CLIENT_HANDLE
#define WMTR_F_CLIENT_HANDLE 0
#endif

/* stubs: read every byte of the call's params struct, then write all of it; result fields (wmtr_handles.inc)
 * get real objects (+1 results retained, as DXMT returns them) */
static volatile uint8_t sink;
static id result_objs[8];
static uint32_t result_n;
static void stub_results(int call, uint8_t *b) {
  const struct wmtr_hfield *f = wmtr_call_hfields[call];
  for (; f && f->flags != WMTR_HF_END; f++) {
    if (f->flags != WMTR_HF_OUT0 && f->flags != WMTR_HF_OUT1) continue;
    id o = (result_n++ % 3) ? result_objs[result_n % 8] : nil;
    if (o && f->flags == WMTR_HF_OUT1) [o retain];
    uint64_t v = (uint64_t)(uintptr_t)o;
    memcpy(b + f->off, &v, 8);
  }
}
#define X(i, n, t, c)                                                                                                \
  static NTSTATUS stub_##i(void *p) {                                                                                \
    uint8_t *b = p, x = 0;                                                                                            \
    for (size_t k = 0; k < sizeof(t); k++) x ^= b[k];                                                                 \
    sink = x;                                                                                                         \
    memset(b, 0, sizeof(t));                                                                                          \
    stub_results(i, b);                                                                                               \
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

/* the seeded table: real objects of several kinds (the stubs never touch them; the executor's own calls do) */
static uint64_t seeded[32];
static uint32_t n_seeded;
static void seed(id o) { if (o && n_seeded < 32) seeded[n_seeded++] = wmtr_host_debug_handle(H, (uint64_t)(uintptr_t)o, 0, 0); }

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
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    seed(dev); seed(q); seed([[q commandBuffer] retain]); seed([dev newBufferWithLength:4096 options:0]);
    seed([dev newTextureWithDescriptor:td]); seed([dev newSharedEvent]); seed([dev newFence]); seed([dev newEvent]);
    seed([[NSString alloc] initWithUTF8String:"label"]); seed([[NSObject alloc] init]);
    for (int i = 0; i < 8; i++) result_objs[i] = i < 4 ? [[NSObject alloc] init] : [dev newBufferWithLength:4096 options:0];
  }
  /* input: call, flags, params_size, n_reloc, n_out, payload_size, then the record body as is */
  struct { uint16_t call, flags; uint32_t params_size; uint16_t n_reloc, n_out; uint32_t payload_size; } in;
  if (size < sizeof in || size > 256 * 1024) return 0;
  memcpy(&in, data, sizeof in);
  data += sizeof in; size -= sizeof in;
  struct wmtr_req_ext hdr = {0};
  hdr.h.flags = WMTR_F_SYNC | (in.flags & (WMTR_F_CONTROL | WMTR_F_CLIENT_HANDLE));
  hdr.h.call = hdr.h.flags & WMTR_F_CONTROL ? in.call % 6 : in.call % (WMTR_NCALLS + 2);
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
  /* handle words: "WO" with bit 40 clear -> a valid seeded handle, so every field sees real handles of every kind */
  for (uint32_t o = sizeof hdr; o + 8 <= total; o += 8) {
    uint64_t w;
    memcpy(&w, ring_data + pos + o, 8);
    if ((w >> 48) == 0x574F && !(w & (1ull << 40)) && n_seeded) {
      w = seeded[(w & 0xff) % n_seeded];
      memcpy(ring_data + pos + o, &w, 8);
    }
  }
  __atomic_store_n(&R->head, head + total, __ATOMIC_RELEASE);
  wmtr_host_kick(H);
  while (__atomic_load_n(&slot->done_seq, __ATOMIC_ACQUIRE) < hdr.h.seq) sched_yield();
  if (getenv("WMTR_FUZZ_STATS")) { /* what the requests ended in: executed, refused handle, malformed */
    static uint64_t n_ok, n_handle, n_other, n;
    uint32_t st = slot->status;
    if (st == 0) n_ok++; else if (st == 0xC0000008) n_handle++; else n_other++;
    if (++n % 100000 == 0) fprintf(stderr, "fuzz stats: %llu ok, %llu refused handle, %llu other\n", n_ok, n_handle, n_other);
  }
  return 0;
}
