/* wmtr_call_bench: per-call cost of the wmtr client + host executor, without Metal work. Client and executor in
 * one process over an in-memory transport (as tests/wmtr_loopback.m); the implementations are no-ops that touch
 * their params, so the times are marshalling, the ring, relocation and handle translation only.
 *
 *   sync    N synchronous calls with one handle (MTLTexture_pixelFormat): the round trip
 *   frame   a d3d11bench-shaped frame: 16 render passes x 50 draws (pool, render encoder, one encodeCommands of
 *           200 commands / 150 handles per pass, endEncoding; commit, present, one synchronous call)
 *
 * Builds against executors with and without object handles (wmtr_handles.h), so the same program measures both:
 *   make OUT=build build/wmtr_call_bench && ./build/wmtr_call_bench [frames] [sync calls] */
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define WINEMETAL_API
#include "winemetal_thunks.h"
#include "airconv_thunks.h"
#include "wmtr_wire.h"
#include "wmtr_transport.h"
#include "wmtr_host.h"
#if __has_include("wmtr_handles.h")
#include "wmtr_handles.h"
extern uint64_t wmtr_host_debug_handle(struct wmtr_host *h, uint64_t obj, int kind, int plus1);
#define HANDLE(obj, kind) wmtr_host_debug_handle(H, (uint64_t)(uintptr_t)(obj), WMTR_K_##kind, 0)
#define VARIANT "object handles"
#else
#define HANDLE(obj, kind) ((uint64_t)(uintptr_t)(obj))
#define VARIANT "raw pointers"
#endif

typedef int NTSTATUS;
typedef NTSTATUS (*fn_t)(void *);
extern const void *__wine_unix_call_funcs[];

static volatile uint64_t sink;
static NTSTATUS f_touch(void *a) { sink += *(uint64_t *)a; return 0; }
static NTSTATUS f_pixelformat(void *a) { ((struct unixcall_generic_obj_uint64_ret *)a)->ret = 70; return 0; }
static NTSTATUS f_encode(void *a) { /* walks the list like DXMT's encodeCommands, reading each handle */
  struct unixcall_generic_obj_cmd_noret *p = a;
  for (const struct wmtcmd_base *c = p->cmd_head.ptr; c; c = c->next.ptr) sink += c->type;
  return 0;
}
static NSObject *enc_obj;
static NTSTATUS f_render_enc(void *a) { ((struct unixcall_generic_obj_uint64_obj_ret *)a)->ret = (obj_handle_t)enc_obj; return 0; }
const void *wmtr_impl_funcs[WMTR_NCALLS] = {[39] = f_pixelformat, [38] = f_encode, [32] = f_render_enc, [33] = f_touch,
                                            [12] = f_touch, [47] = f_touch};

static struct wmtr_host *H;
static void *blob_base[WMTR_MAX_BLOBS]; static uint64_t blob_size[WMTR_MAX_BLOBS];
static void *t_blob_ptr(void *c, uint32_t b, uint64_t *s) { if (b >= WMTR_MAX_BLOBS) return NULL; *s = blob_size[b]; return blob_base[b]; }
static void t_wake(void *c, uint32_t s) {}
static const struct wmtr_host_ops host_ops = {t_blob_ptr, t_wake, NULL, NULL, NULL};
static void *t_open_ring(struct wmtr_transport *t, uint64_t size) {
  struct wmtr_ring *R; posix_memalign((void **)&R, WMTR_PAGE, size); memset(R, 0, size);
  R->magic = WMTR_RING_MAGIC; R->version = WMTR_VERSION;
  blob_base[0] = R; blob_size[0] = size;
  H = wmtr_host_create(R, &host_ops, NULL);
  wmtr_host_set_spin(H, 1000000000ull); /* the executor polls: measure work, not wake-ups */
  return R;
}
static void *t_add_blob(struct wmtr_transport *t, uint32_t b, uint64_t size) {
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (p == MAP_FAILED) return NULL;
  blob_base[b] = p; blob_size[b] = size; return p;
}
static void t_kick(struct wmtr_transport *t) { wmtr_host_kick(H); }
static void t_wait(struct wmtr_transport *t, volatile uint64_t *d, uint64_t seq, uint32_t slot, uint64_t spin_ns) {
  while (__atomic_load_n(d, __ATOMIC_ACQUIRE) < seq) __asm__ __volatile__("yield");
}
struct wmtr_transport *wmtr_transport_create(void) {
  struct wmtr_transport *t = calloc(1, sizeof *t);
  t->open_ring = t_open_ring; t->add_blob = t_add_blob; t->kick = t_kick; t->wait = t_wait;
  return t;
}
static NTSTATUS remote(int call, void *p) { return ((fn_t)__wine_unix_call_funcs[call])(p); }
static double ns(uint64_t t) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return (double)t * tb.numer / tb.denom; }

static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
  @autoreleasepool {
    int frames = argc > 1 ? atoi(argv[1]) : 3000, nsync = argc > 2 ? atoi(argv[2]) : 200000;
    enc_obj = [[NSObject alloc] init];
    struct unixcall_generic_obj_uint64_ret q = {0, 0};
    remote(39, &q); /* connects */
    static NSObject *objs[64];
    for (int i = 0; i < 64; i++) objs[i] = [[NSObject alloc] init];
    uint64_t tex = HANDLE(objs[0], TEXTURE), cmdbuf = HANDLE(objs[1], CMDBUF), pso = HANDLE(objs[2], RPSO);
    uint64_t bufs[16], texs[16];
    for (int i = 0; i < 16; i++) { bufs[i] = HANDLE(objs[8 + i], BUFFER); texs[i] = HANDLE(objs[24 + i], TEXTURE); }

    /* sync round trips */
    double *lat = malloc(sizeof(double) * nsync);
    for (int i = 0; i < nsync; i++) {
      q.handle = tex;
      uint64_t t0 = mach_absolute_time();
      remote(39, &q);
      lat[i] = ns(mach_absolute_time() - t0);
    }
    qsort(lat, nsync, sizeof *lat, cmp);
    double sum = 0; for (int i = 0; i < nsync; i++) sum += lat[i];

    /* frames */
    enum { PASSES = 16, DRAWS = 50 };
    static struct wmtcmd_render_setpso c_pso[DRAWS];
    static struct wmtcmd_render_setbuffer c_vb[DRAWS];
    static struct wmtcmd_render_settexture c_tx[DRAWS];
    static struct wmtcmd_render_draw c_dr[DRAWS];
    for (int d = 0; d < DRAWS; d++) {
      memset(&c_pso[d], 0, sizeof c_pso[d]); memset(&c_vb[d], 0, sizeof c_vb[d]);
      memset(&c_tx[d], 0, sizeof c_tx[d]); memset(&c_dr[d], 0, sizeof c_dr[d]);
      c_pso[d].type = WMTRenderCommandSetPSO; c_pso[d].pso = pso; c_pso[d].next.ptr = &c_vb[d];
      c_vb[d].type = WMTRenderCommandSetVertexBuffer; c_vb[d].buffer = bufs[d % 16]; c_vb[d].next.ptr = &c_tx[d];
      c_tx[d].type = WMTRenderCommandSetFragmentTexture; c_tx[d].texture = texs[d % 16]; c_tx[d].next.ptr = &c_dr[d];
      c_dr[d].type = WMTRenderCommandDraw; c_dr[d].vertex_count = 3; c_dr[d].instance_count = 1;
      c_dr[d].next.ptr = d + 1 < DRAWS ? (void *)&c_pso[d + 1] : NULL;
    }
    struct WMTRenderPassInfo rpi; memset(&rpi, 0, sizeof rpi);
    rpi.colors[0].texture = tex; rpi.render_target_width = 1280; rpi.render_target_height = 800;
    double *ft = malloc(sizeof(double) * frames);
    for (int f = 0; f < frames; f++) {
      uint64_t t0 = mach_absolute_time();
      for (int p = 0; p < PASSES; p++) {
        struct unixcall_generic_obj_ret pool = {0};
        remote(10, &pool);
        struct unixcall_generic_obj_uint64_obj_ret re = {cmdbuf, (uint64_t)(uintptr_t)&rpi, 0};
        remote(32, &re);
        struct unixcall_generic_obj_cmd_noret enc = {re.ret, {&c_pso[0]}};
        remote(38, &enc);
        struct unixcall_generic_obj_noret end = {re.ret};
        remote(33, &end);
        obj_handle_t ph = pool.ret;
        remote(1, &ph);
      }
      struct unixcall_generic_obj_noret cm = {cmdbuf};
      remote(12, &cm);
      struct unixcall_generic_obj_obj_noret pd = {cmdbuf, 0};
      remote(47, &pd);
      q.handle = tex;
      remote(39, &q); /* the frame's synchronous call: everything above has executed */
      ft[f] = ns(mach_absolute_time() - t0);
    }
    qsort(ft, frames, sizeof *ft, cmp);
    double fsum = 0; for (int i = 0; i < frames; i++) fsum += ft[i];
    printf("RESULT variant=\"%s\" sync_mean_ns=%.0f sync_p50_ns=%.0f sync_p99_ns=%.0f frame_mean_us=%.1f frame_p50_us=%.1f "
           "frame_p99_us=%.1f per_command_ns=%.1f\n",
           VARIANT, sum / nsync, lat[nsync / 2], lat[nsync * 99 / 100], fsum / frames / 1000, ft[frames / 2] / 1000,
           ft[frames * 99 / 100] / 1000, ft[frames / 2] / (PASSES * DRAWS * 4.0));
  }
  return 0;
}
