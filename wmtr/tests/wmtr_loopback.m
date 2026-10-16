/* wmtr_loopback: client and host executor in one process over an in-memory transport.
 * Oracle: each test calls a winemetal entry once directly (the native path: the implementation
 * reads the caller's memory) and once through __wine_unix_call_funcs of the wmtr client (params
 * relocated into the ring, executed by the host, outputs copied back). Results must be identical.
 * The implementations are small fakes with the real param layouts, plus real Metal for newBuffer. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <libproc.h>
#include <sys/mman.h>
#include <time.h>

#define WINEMETAL_API
#include "winemetal_thunks.h"
#include "airconv_thunks.h"
#include "wmtr_proto.h"
#include "wmtr_transport.h"
#include "wmtr_host.h"

typedef int NTSTATUS;
typedef NTSTATUS (*fn_t)(void *);
extern const void *__wine_unix_call_funcs[];

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---------------- fake implementations (the "host Metal") ---------------- */
static NTSTATUS f_getcstring(void *a) {
  struct unixcall_nsstring_getcstring *p = a;
  const char *s = (const char *)p->str; /* the fake NSString handle is a C string */
  strncpy((char *)(uintptr_t)p->buffer_ptr, s, p->max_length);
  p->ret = 1;
  return 0;
}
static uint64_t encode_sum; static uint32_t encode_cmds, encode_vp;
static NTSTATUS f_render_encode(void *a) {
  struct unixcall_generic_obj_cmd_noret *p = a;
  for (const struct wmtcmd_base *c = p->cmd_head.ptr; c; c = c->next.ptr) {
    encode_cmds++;
    if (c->type == WMTRenderCommandSetFragmentBytes) {
      const struct wmtcmd_render_setbytes *b = (const void *)c;
      for (uint64_t i = 0; i < b->length; i++) encode_sum += ((const uint8_t *)b->bytes.ptr)[i];
    } else if (c->type == WMTRenderCommandSetViewports) {
      const struct wmtcmd_render_setviewports *v = (const void *)c;
      for (int i = 0; i < v->viewport_count; i++) encode_vp += (uint32_t)((const struct WMTViewport *)v->viewports.ptr)[i].width;
    }
  }
  return 0;
}
static NTSTATUS f_getbytes(void *a) {
  struct unixcall_mtltexture_replaceregion *p = a;
  uint8_t *d = p->data.ptr;
  for (uint64_t i = 0; i < p->bytes_per_row * p->size.height; i++) d[i] = (uint8_t)(i * 7 + p->level);
  return 0;
}
static uint64_t replace_sum;
static NTSTATUS f_replace(void *a) {
  struct unixcall_mtltexture_replaceregion *p = a;
  const uint8_t *d = p->data.ptr;
  for (uint64_t i = 0; i < p->bytes_per_row * p->size.height; i++) replace_sum += d[i] * (i + 1);
  return 0;
}
static NTSTATUS f_argsinfo(void *a) { (void)a; return 0; }
static NTSTATUS f_noop(void *a) { (void)a; return 0; }
static NTSTATUS f_fault(void *a) { (void)a; return *(volatile int *)(uintptr_t)0x10; }
/* an autoreleased result whose lifetime the test watches */
static int probe_alive;
@interface Probe : NSObject
@end
@implementation Probe
- (instancetype)init { if ((self = [super init])) probe_alive = 1; return self; }
- (void)dealloc { probe_alive = 0; [super dealloc]; }
@end
static NTSTATUS f_autoreleased(void *a) { ((struct unixcall_generic_obj_ret *)a)->ret = (obj_handle_t)[[[Probe alloc] init] autorelease]; return 0; }
/* DXMT's out-errors: autoreleased NSError retained before the handle is returned (+1, owned by the caller) */
static int err_alive;
@interface ErrProbe : NSObject
@end
@implementation ErrProbe
- (instancetype)init { if ((self = [super init])) err_alive = 1; return self; }
- (void)dealloc { err_alive = 0; [super dealloc]; }
@end
static NTSTATUS f_newlibrary(void *a) {
  struct unixcall_mtldevice_newlibrary *p = a;
  p->ret_library = 0;
  p->ret_error = (obj_handle_t)[[[[ErrProbe alloc] init] autorelease] retain];
  return 0;
}
/* SM50Compile: walks the argument list the way airconv does; sums each root signature's bytecode */
static uint64_t sm50_rs_sum[2]; static uint32_t sm50_rs_len[2];
static NTSTATUS f_sm50compile(void *a) {
  struct sm50_compile_params *p = a;
  memset(sm50_rs_sum, 0, sizeof sm50_rs_sum); memset(sm50_rs_len, 0, sizeof sm50_rs_len);
  for (struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *g = p->args; g; g = g->next) {
    if (g->type != SM50_SHADER_ROOT_SIGNATURE && g->type != SM50_SHADER_ROOT_SIGNATURE2) continue;
    struct SM50_SHADER_ROOT_SIGNATURE_DATA *d = (void *)g;
    int k = g->type == SM50_SHADER_ROOT_SIGNATURE2;
    sm50_rs_len[k] = (uint32_t)d->bytecode_length;
    for (size_t i = 0; i < d->bytecode_length; i++) sm50_rs_sum[k] = sm50_rs_sum[k] * 31 + ((const uint8_t *)d->bytecode)[i];
  }
  *p->bitcode = (sm50_bitcode_t)(uintptr_t)0x5150; p->ret = 0;
  return 0;
}
static NTSTATUS f_pixelformat(void *a) { ((struct unixcall_generic_obj_uint64_ret *)a)->ret = 70; return 0; } /* RGBA8 */
static NTSTATUS f_release(void *a) { [(id)*(obj_handle_t *)a release]; return 0; }
static NTSTATUS f_newfunction(void *a) {
  struct unixcall_generic_obj_uint64_obj_ret *p = a;
  p->ret = strlen((const char *)(uintptr_t)p->arg) * 1000 + p->handle;
  return 0;
}
/* encoders: autoreleased objects; the log records (call, receiver or result) in execution order */
static uint64_t enc_log[64][2]; static int n_enc_log;
static void enc_note(uint64_t call, uint64_t v) { if (n_enc_log < 64) { enc_log[n_enc_log][0] = call; enc_log[n_enc_log][1] = v; } n_enc_log++; }
static NTSTATUS f_new_encoder(uint64_t *ret, uint64_t call, int fail) {
  *ret = fail ? 0 : (obj_handle_t)[[[NSObject alloc] init] autorelease];
  enc_note(call, *ret);
  return 0;
}
static NTSTATUS f_blit_enc(void *a) { return f_new_encoder(&((struct unixcall_generic_obj_obj_ret *)a)->ret, 30, 0); }
static NTSTATUS f_compute_enc(void *a) { struct unixcall_generic_obj_uint64_obj_ret *p = a; return f_new_encoder(&p->ret, 31, p->arg == 99); }
static NTSTATUS f_render_enc(void *a) {
  struct unixcall_generic_obj_uint64_obj_ret *p = a;
  const struct WMTRenderPassInfo *i = (const void *)(uintptr_t)p->arg;
  return f_new_encoder(&p->ret, 32, !i || i->render_target_width != 640);
}
static NTSTATUS f_end_encoding(void *a) { enc_note(33, ((struct unixcall_generic_obj_noret *)a)->handle); return 0; }
static NTSTATUS f_compute_encode(void *a) { enc_note(37, ((struct unixcall_generic_obj_cmd_noret *)a)->encoder); return 0; }
const void *wmtr_impl_funcs[146] = {[1] = f_release, [25] = f_newlibrary, [76] = f_sm50compile, [30] = f_blit_enc, [31] = f_compute_enc, [32] = f_render_enc,
                                    [33] = f_end_encoding, [37] = f_compute_encode, [8] = f_getcstring, [26] = f_newfunction, [38] = f_render_encode,
                                    [39] = f_pixelformat, [47] = f_noop, [53] = f_autoreleased, [5] = f_fault, [88] = f_argsinfo, [45] = f_replace, [145] = f_getbytes};

/* ---------------- loopback transport ---------------- */
static struct wmtr_host *H;
static void *blob_base[WMTR_MAX_BLOBS]; static uint64_t blob_size[WMTR_MAX_BLOBS];
static void *t_blob_ptr(void *c, uint32_t b, uint64_t *s) { *s = blob_size[b]; return blob_base[b]; }
static void t_wake(void *c, uint32_t s) {}
static uint64_t next_view = 0x1000, released[8]; static int n_released;
static uint64_t t_create_view(void *c, uint64_t hwnd, void *dev, uint64_t *layer) { *layer = 0x77; return next_view++; }
static void t_release_view(void *c, uint64_t v) { if (n_released < 8) released[n_released] = v; n_released++; }
static const struct wmtr_host_ops host_ops = {t_blob_ptr, t_wake, t_create_view, t_release_view, NULL};
static void *pages(uint64_t n) { void *p; posix_memalign(&p, WMTR_PAGE, n); memset(p, 0, n); return p; }
static void *t_open_ring(struct wmtr_transport *t, uint64_t size) {
  struct wmtr_ring *R = pages(size);
  R->magic = WMTR_RING_MAGIC; R->version = WMTR_VERSION;
  blob_base[0] = R; blob_size[0] = size;
  H = wmtr_host_create(R, &host_ops, NULL);
  return R;
}
/* arena blobs are their own mappings, like the shm blobs of the real transports */
static void *t_add_blob(struct wmtr_transport *t, uint32_t b, uint64_t size) {
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (p == MAP_FAILED) return NULL;
  blob_base[b] = p; blob_size[b] = size; return p;
}
static int n_blob_released;
static void t_release_blob(struct wmtr_transport *t, uint32_t b, void *base, uint64_t size) {
  if (base == blob_base[b] && size == blob_size[b]) { munmap(blob_base[b], size); blob_base[b] = NULL; blob_size[b] = 0; n_blob_released++; }
}
static uint64_t arena_bytes(void) { uint64_t n = 0; for (int b = 1; b < WMTR_MAX_BLOBS; b++) n += blob_base[b] ? blob_size[b] : 0; return n; }
static uint64_t footprint(void) { struct rusage_info_v2 ri; return proc_pid_rusage(getpid(), RUSAGE_INFO_V2, (rusage_info_t *)&ri) ? 0 : ri.ri_phys_footprint; }
static void t_kick(struct wmtr_transport *t) { wmtr_host_kick(H); }
static void t_wait(struct wmtr_transport *t, volatile uint64_t *d, uint64_t seq, uint32_t slot, uint64_t spin_ns) { (void)spin_ns; while (__atomic_load_n(d, __ATOMIC_ACQUIRE) < seq) sched_yield(); }
struct wmtr_transport *wmtr_transport_create(void) {
  struct wmtr_transport *t = calloc(1, sizeof *t);
  t->open_ring = t_open_ring; t->add_blob = t_add_blob; t->kick = t_kick; t->wait = t_wait;
  t->release_blob = t_release_blob;
  return t;
}
static NTSTATUS remote(int call, void *p) { return ((fn_t)__wine_unix_call_funcs[call])(p); }
static NTSTATUS native(int call, void *p) { return ((fn_t)wmtr_impl_funcs[call])(p); }

static char frames_path[64];
int main(void) {
  @autoreleasepool {
    snprintf(frames_path, sizeof frames_path, "/tmp/wmtr_frames_%d.csv", (int)getpid());
    unlink(frames_path);
    setenv("WMTR_FRAMES", frames_path, 1);
    setenv("WMTR_ARENA_TRIM_MS", "300", 1); /* section 7 */
    char stats_path[] = "/tmp/wmtr_callstats_XXXXXX";
    close(mkstemp(stats_path));
    /* WMTR_TEST_TRIGGER=1: statistics turned on at runtime through the trigger file instead of the environment */
    int trigger_mode = getenv("WMTR_TEST_TRIGGER") != NULL;
    char trigger_path[64];
    snprintf(trigger_path, sizeof trigger_path, "/tmp/wmtr_trigger_%d", (int)getpid());
    if (trigger_mode) {
      unsetenv("WMTR_CALLSTATS");
      setenv("WMTR_CALLSTATS_TRIGGER", trigger_path, 1);
      setenv("WMTR_CALLSTATS_POLL_MS", "20", 1);
      FILE *tf = fopen(trigger_path, "w");
      fprintf(tf, "%s\n", stats_path);
      fclose(tf);
    } else {
      setenv("WMTR_CALLSTATS", stats_path, 1);
    }
    /* 1. output buffer through a uint64 pointer field */
    char a[32] = {0}, b[32] = {0};
    struct unixcall_nsstring_getcstring pa = {(obj_handle_t)"metal string", (uint64_t)(uintptr_t)a, sizeof a, 4, 0}, pb = pa;
    pb.buffer_ptr = (uint64_t)(uintptr_t)b;
    native(8, &pa); remote(8, &pb);
    CHECK(!strcmp(a, b) && pb.ret == 1 && pb.buffer_ptr == (uint64_t)(uintptr_t)b, "getCString: '%s' vs '%s'", a, b);
    if (trigger_mode) usleep(200000); /* the first call started the statistics thread; it sees the trigger */

    /* 2. C-string input in a uint64 arg, value result */
    struct unixcall_generic_obj_uint64_obj_ret fa = {5, (uint64_t)(uintptr_t)"vertex_main", 0}, fb = fa;
    native(26, &fa); remote(26, &fb);
    CHECK(fa.ret == fb.ret, "newFunction ret %llu vs %llu", fa.ret, fb.ret);

    /* 3. command list with embedded pointers (async call, then a sync call orders it) */
    struct WMTViewport vps[3] = {{0, 0, 640, 480, 0, 1}, {0, 0, 320, 240, 0, 1}, {0, 0, 7, 7, 0, 1}};
    uint8_t bytes[300]; for (int i = 0; i < 300; i++) bytes[i] = (uint8_t)(i * 13);
    struct wmtcmd_render_setviewports cv = {0}; cv.type = WMTRenderCommandSetViewports; cv.viewports.ptr = vps; cv.viewport_count = 3;
    struct wmtcmd_render_setbytes cbytes = {0}; cbytes.type = WMTRenderCommandSetFragmentBytes; cbytes.bytes.ptr = bytes; cbytes.length = 300; cbytes.index = 2;
    struct wmtcmd_base nop = {0}; nop.type = WMTRenderCommandNop;
    cv.next.ptr = &cbytes; cbytes.next.ptr = &nop; nop.next.ptr = NULL;
    struct unixcall_generic_obj_cmd_noret ce = {0x1234, {&cv}};
    native(38, &ce);
    uint64_t n_sum = encode_sum; uint32_t n_cmds = encode_cmds, n_vp = encode_vp;
    encode_sum = 0; encode_cmds = 0; encode_vp = 0;
    remote(38, &ce);
    remote(26, &fb); /* sync: the async encode before it has executed */
    CHECK(n_sum == encode_sum && n_cmds == encode_cmds && n_vp == encode_vp, "encode sum %llu/%llu cmds %u/%u vp %u/%u",
          n_sum, encode_sum, n_cmds, encode_cmds, n_vp, encode_vp);

    /* 3b. a command list longer than any fixed relocation budget (DXMT batches ~100+ draws per call) */
    {
      enum { N = 2000 };
      static struct wmtcmd_render_setbytes cmds[N]; static uint8_t payload[N][16];
      for (int i = 0; i < N; i++) {
        memset(&cmds[i], 0, sizeof cmds[i]); cmds[i].type = WMTRenderCommandSetFragmentBytes;
        for (int k = 0; k < 16; k++) payload[i][k] = (uint8_t)(i + k);
        cmds[i].bytes.ptr = payload[i]; cmds[i].length = 16; cmds[i].next.ptr = i + 1 < N ? &cmds[i + 1] : NULL;
      }
      struct unixcall_generic_obj_cmd_noret cl = {0x1234, {&cmds[0]}};
      encode_sum = 0; encode_cmds = 0; native(38, &cl);
      uint64_t s1 = encode_sum; uint32_t c1 = encode_cmds;
      encode_sum = 0; encode_cmds = 0; NTSTATUS st = remote(38, &cl); remote(26, &fb);
      CHECK(st == 0 && s1 == encode_sum && c1 == encode_cmds, "long list st=%x sum %llu/%llu cmds %u/%u", st, s1, encode_sum, c1, encode_cmds);
    }
    /* 3c. zero-length output array (SM50GetArgumentsInfo of a shader without constant buffers) */
    {
      struct MTL_SM50_SHADER_ARGUMENT none[1];
      struct sm50_get_arguments_info_params ga = {(sm50_shader_t)0x77, none, none};
      NTSTATUS st = remote(88, &ga);
      CHECK(st == 0 && ga.constant_buffers == none, "zero-length output st=%x", st);
    }

    /* 4. large input (arena staging) and large output (arena) */
    uint32_t W = 1024, Hh = 512; uint8_t *src = malloc(W * 4 * Hh), *o1 = malloc(W * 4 * Hh), *o2 = malloc(W * 4 * Hh);
    for (uint32_t i = 0; i < W * 4 * Hh; i++) src[i] = (uint8_t)(i ^ (i >> 9));
    struct unixcall_mtltexture_replaceregion rr = {0}; rr.texture = 0x9000; rr.size = (struct WMTSize){W, Hh, 1};
    rr.data.ptr = src; rr.bytes_per_row = W * 4; rr.level = 3;
    native(45, &rr); uint64_t n_rs = replace_sum; replace_sum = 0;
    remote(45, &rr); remote(26, &fb);
    CHECK(n_rs == replace_sum, "replaceRegion checksum %llu vs %llu", n_rs, replace_sum);
    rr.data.ptr = o1; native(145, &rr); rr.data.ptr = o2; remote(145, &rr);
    CHECK(!memcmp(o1, o2, W * 4 * Hh) && rr.data.ptr == o2, "getBytes output differs");

    /* 5. newBuffer: client arena memory backs a real MTLBuffer; CPU writes are GPU-visible
          (blit to a private buffer and back); release returns the range through the free queue */
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    struct WMTBufferInfo bi = {100000, WMTResourceStorageModeShared, {NULL}, 0};
    struct unixcall_mtldevice_newbuffer nb = {(obj_handle_t)dev, {&bi}, 0};
    remote(18, &nb);
    CHECK(nb.ret && bi.memory.ptr && bi.gpu_address, "newBuffer ret %llx mem %p gpu %llx", nb.ret, bi.memory.ptr, bi.gpu_address);
    if (nb.ret && bi.memory.ptr) {
      id<MTLBuffer> buf = (id<MTLBuffer>)nb.ret;
      memset(bi.memory.ptr, 0x5a, 100000);
      id<MTLBuffer> tmp = [dev newBufferWithLength:100000 options:MTLResourceStorageModeShared];
      id<MTLCommandQueue> q = [dev newCommandQueue];
      id<MTLCommandBuffer> cb = [q commandBuffer];
      id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
      [be copyFromBuffer:buf sourceOffset:0 toBuffer:tmp destinationOffset:0 size:100000];
      [be fillBuffer:buf range:NSMakeRange(0, 64) value:0x11];
      [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
      CHECK(((uint8_t *)tmp.contents)[99999] == 0x5a, "GPU did not see client writes");
      CHECK(((uint8_t *)bi.memory.ptr)[10] == 0x11, "client did not see GPU writes");
      [tmp release]; [q release];
      struct wmtr_ring *R = blob_base[0];
      uint64_t fh = R->free_head;
      obj_handle_t h = nb.ret;
      remote(1, &h); remote(26, &fb);
      for (int i = 0; i < 1000 && R->free_head == fh; i++) usleep(1000);
      CHECK(R->free_head == fh + 1, "buffer range not returned (free_head %llu)", R->free_head);
    }
    /* 6. caller-placed memory (DXMT CpuPlaced): initial bytes kept, pointer replaced by arena memory */
    {
      uint8_t *own; posix_memalign((void **)&own, 16384, 65536); memset(own, 0x3c, 65536);
      struct WMTBufferInfo ci = {65536, WMTResourceStorageModeShared, {own}, 0};
      struct unixcall_mtldevice_newbuffer cb2 = {(obj_handle_t)dev, {&ci}, 0};
      remote(18, &cb2);
      CHECK(cb2.ret && ci.memory.ptr && ci.memory.ptr != own && ((uint8_t *)ci.memory.ptr)[65535] == 0x3c,
            "caller-placed buffer ret %llx mem %p", cb2.ret, ci.memory.ptr);
      free(own);
    }
    /* 7. after a peak, idle arena blobs beyond the 128 MiB reserve go back to the host once they have
          been idle for WMTR_ARENA_TRIM_MS; blobs reused within that time are kept */
    {
      enum { NB = 8 };
      struct wmtr_ring *R = blob_base[0];
      uint64_t a0 = arena_bytes();
      obj_handle_t bh[NB];
      for (int i = 0; i < NB; i++) {
        struct WMTBufferInfo pi = {60u << 20, WMTResourceStorageModeShared, {NULL}, 0};
        struct unixcall_mtldevice_newbuffer pb = {(obj_handle_t)dev, {&pi}, 0};
        remote(18, &pb);
        bh[i] = pb.ret;
        if (pi.memory.ptr) memset(pi.memory.ptr, i + 1, 60u << 20);
      }
      uint64_t a_peak = arena_bytes(), fp_peak = footprint();
      CHECK(bh[NB - 1] && a_peak >= a0 + (NB - 1) * (60ull << 20), "peak: %llu arena MiB", a_peak >> 20);
      uint64_t fh = R->free_head;
      for (int i = 0; i < NB; i++) remote(1, &bh[i]);
      remote(26, &fb);
      for (int i = 0; i < 2000 && R->free_head < fh + NB; i++) usleep(1000);
      remote(26, &fb); /* takes the frees: the blobs are idle, but not for long enough yet */
      CHECK(n_blob_released == 0 && arena_bytes() == a_peak, "released %d blobs before the idle time", n_blob_released);
      usleep(400000);
      remote(26, &fb);
      uint64_t a_after = arena_bytes(), fp_after = footprint();
      printf("arena trim: arena %llu -> %llu MiB, footprint %llu -> %llu MiB, %d blobs released\n",
             a_peak >> 20, a_after >> 20, fp_peak >> 20, fp_after >> 20, n_blob_released);
      CHECK(a_after <= a0 + (128ull << 20), "arena kept %llu MiB after the peak (before %llu MiB)", a_after >> 20, a0 >> 20);
      CHECK(fp_after + (256ull << 20) <= fp_peak, "footprint %llu -> %llu MiB", fp_peak >> 20, fp_after >> 20);
      /* the arena still works: a released blob number is reused */
      struct WMTBufferInfo ri = {100u << 20, WMTResourceStorageModeShared, {NULL}, 0};
      struct unixcall_mtldevice_newbuffer rb = {(obj_handle_t)dev, {&ri}, 0};
      remote(18, &rb);
      CHECK(rb.ret && ri.memory.ptr, "newBuffer after trim");
      if (rb.ret) { memset(ri.memory.ptr, 7, 100u << 20); obj_handle_t h = rb.ret; remote(1, &h); }
    }
    /* 8. client-assigned handles: a frame's pool + encoder creations do not wait for the host (one sync call
          apart from the calls that return values); each use reaches the host after its creation and gets the object;
          a failed creation makes the calls using its handle no-ops. WMTR_CLIENT_HANDLES=0: every creation waits. */
    {
      int on = !getenv("WMTR_CLIENT_HANDLES") || atoi(getenv("WMTR_CLIENT_HANDLES"));
      struct wmtr_host_stats s0, s1;
      wmtr_host_get_stats(H, &s0);
      n_enc_log = 0;
      struct unixcall_generic_obj_ret pool = {0};
      remote(10, &pool);
      struct unixcall_generic_obj_ret ar = {0};
      remote(53, &ar); /* autoreleased into the pool */
      struct WMTRenderPassInfo rpi; memset(&rpi, 0, sizeof rpi); rpi.render_target_width = 640;
      struct unixcall_generic_obj_uint64_obj_ret re = {0x77, (uint64_t)(uintptr_t)&rpi, 0}, ce = {0x77, 1, 0}, bad = {0x77, 99, 0};
      struct unixcall_generic_obj_obj_ret be = {0x77, 0};
      remote(32, &re);
      struct unixcall_generic_obj_noret end = {re.ret}; remote(33, &end);
      remote(31, &ce);
      struct unixcall_generic_obj_cmd_noret enc = {ce.ret, {NULL}}; remote(37, &enc);
      end.handle = ce.ret; remote(33, &end);
      remote(30, &be);
      end.handle = be.ret; remote(33, &end);
      remote(31, &bad); /* fails on the host */
      enc.encoder = bad.ret; remote(37, &enc);
      end.handle = bad.ret; remote(33, &end);
      obj_handle_t ph = pool.ret;
      int alive_in_pool = 0;
      struct unixcall_generic_obj_uint64_ret q = {0}; remote(39, &q); /* sync: all of the above has executed */
      alive_in_pool = probe_alive;
      remote(1, &ph);
      remote(39, &q);
      wmtr_host_get_stats(H, &s1);
      uint64_t waits = s1.sync_requests - s0.sync_requests;
      CHECK(waits == (on ? 3u : 8u), "pool+encoders: %llu sync requests (want %d)", waits, on ? 3 : 8); /* 53 and 39 twice */
      CHECK(pool.ret && re.ret && ce.ret && be.ret && (on ? bad.ret != 0 : bad.ret == 0), "handles %llx %llx %llx %llx %llx",
            pool.ret, re.ret, ce.ret, be.ret, bad.ret);
      CHECK(alive_in_pool && !probe_alive, "pool handle: object alive in pool %d, after release %d", alive_in_pool, probe_alive);
      /* host order: 32 r, 33 r, 31 c, 37 c, 33 c, 30 b, 33 b, 31 0 (failed; its uses are skipped) */
      static const uint64_t want[8] = {32, 33, 31, 37, 33, 30, 33, 31};
      int ok = n_enc_log == (on ? 8 : 10);
      for (int i = 0; ok && i < 8; i++) ok = enc_log[i][0] == want[i];
      ok = ok && enc_log[0][1] && enc_log[1][1] == enc_log[0][1] && enc_log[3][1] == enc_log[2][1] &&
           enc_log[4][1] == enc_log[2][1] && enc_log[6][1] == enc_log[5][1] && enc_log[7][1] == 0;
      CHECK(ok, "encoder order/objects: %d log entries", n_enc_log);
      CHECK(end.handle == bad.ret, "the caller's receiver handle was changed to %llx", end.handle);
    }
    struct wmtr_host_stats st; wmtr_host_get_stats(H, &st);
    printf("requests=%llu sync=%llu bytes=%llu\n", st.requests, st.sync_requests, st.bytes);
    { /* an autoreleased result outside any client pool must outlive the request (DXMT reads it next) */
      struct unixcall_generic_obj_ret ar = {0};
      remote(53, &ar);
      struct unixcall_generic_obj_uint64_ret pf3 = {0}; remote(39, &pf3);
      CHECK(ar.ret && probe_alive, "autoreleased result without a pool was freed with the request");
    }
    { /* D3D12 pipelines pass serialized root signatures through SM50Compile's argument list */
      uint8_t rs1[300], rs2[77]; for (int i = 0; i < 300; i++) rs1[i] = (uint8_t)(i * 7 + 1); for (int i = 0; i < 77; i++) rs2[i] = (uint8_t)(i ^ 0x5a);
      struct SM50_SHADER_ROOT_SIGNATURE_DATA r2 = {.next = NULL, .type = SM50_SHADER_ROOT_SIGNATURE2, .bytecode = rs2, .bytecode_length = 77};
      struct SM50_SHADER_ROOT_SIGNATURE_DATA r1 = {.next = &r2, .type = SM50_SHADER_ROOT_SIGNATURE, .bytecode = rs1, .bytecode_length = 300};
      struct SM50_SHADER_COMMON_DATA cm = {0}; cm.next = &r1; cm.type = SM50_SHADER_COMMON;
      sm50_bitcode_t bc = 0; sm50_error_t er = 0;
      struct sm50_compile_params cp = {.shader = 0, .args = &cm, .func_name = "main", .bitcode = &bc, .error = &er};
      native(76, &cp); uint64_t n0 = sm50_rs_sum[0], n1 = sm50_rs_sum[1];
      bc = 0; NTSTATUS st = remote(76, &cp);
      CHECK(st == 0 && bc == (sm50_bitcode_t)(uintptr_t)0x5150 && sm50_rs_len[0] == 300 && sm50_rs_len[1] == 77 &&
            sm50_rs_sum[0] == n0 && sm50_rs_sum[1] == n1, "root signature args: st=%x len %u/%u", st, sm50_rs_len[0], sm50_rs_len[1]);
    }
    { /* a +1 out-error lives exactly as long as the caller holds it: wmtr neither keeps nor drops it */
      struct unixcall_mtldevice_newlibrary nl = {0};
      remote(25, &nl);
      struct unixcall_generic_obj_uint64_ret pf4 = {0}; remote(39, &pf4);
      CHECK(nl.ret_error && err_alive, "out-error freed before its owner released it");
      obj_handle_t eh = nl.ret_error;
      remote(1, &eh);
      remote(39, &pf4);
      CHECK(!err_alive, "out-error still alive after its owner released it (kept by wmtr)");
    }
    { /* WMTR_FRAMES: a presenting command buffer adds one line when it completes */
      id<MTLCommandQueue> q = [MTLCreateSystemDefaultDevice() newCommandQueue];
      id<MTLCommandBuffer> cb = [q commandBuffer];
      struct unixcall_generic_obj_obj_noret pd = {.handle = (obj_handle_t)cb, .arg = 0};
      remote(47, &pd);
      struct unixcall_generic_obj_uint64_ret pf2 = {0}; remote(39, &pf2); /* the async present is done */
      [cb commit]; [cb waitUntilCompleted];
      usleep(100000); /* completion handlers run after waitUntilCompleted returns */
      FILE *f = fopen(frames_path, "r"); char line[128] = ""; int n = 0; unsigned long long t = 0;
      while (f && fgets(line, sizeof line, f)) { n++; sscanf(line, "%llu,", &t); }
      if (f) fclose(f);
      CHECK(n == 1 && t > 0 && strstr(line, ",0,0,0,0,0"), "WMTR_FRAMES: %d lines, last '%s'", n, line);
      unlink(frames_path);
    }
    { /* views: ReleaseMetalView releases once; views the client never released go on destroy */
      struct unixcall_create_metal_view_from_hwnd v1 = {.hwnd = 1, .device = 2}, v2 = {.hwnd = 3, .device = 2};
      CHECK(remote(72, &v1) == 0 && remote(72, &v2) == 0 && v1.ret_view && v2.ret_view && v1.ret_layer == 0x77,
            "CreateMetalViewFromHWND %llx %llx", v1.ret_view, v2.ret_view);
      struct unixcall_generic_obj_noret r1 = {.handle = v1.ret_view};
      remote(73, &r1);
      remote(73, &r1); /* a second release of the same view is ignored */
      struct unixcall_generic_obj_uint64_ret pf = {0}; remote(39, &pf); /* sync call: the releases above are done */
      CHECK(n_released == 1 && released[0] == v1.ret_view, "ReleaseMetalView: %d releases", n_released);
      /* a call that faults (bad handle) fails, the process lives, and the client is refused from then on */
      struct unixcall_generic_obj_uint64_ret fz = {0};
      NTSTATUS fs = remote(5, &fz);
      struct unixcall_generic_obj_uint64_ret pf4 = {0};
      NTSTATUS fs2 = remote(39, &pf4);
      CHECK(fs == (NTSTATUS)0xC0000005 && fs2 == (NTSTATUS)0xC0000005, "fault containment: %x then %x", fs, fs2);
      { /* executor spin budget: with a budget longer than the gaps between requests it never sleeps */
        struct wmtr_host_stats st0, st1, st2;
        wmtr_host_set_spin(H, 0);
        wmtr_host_get_stats(H, &st0);
        for (int i = 0; i < 20; i++) { struct unixcall_generic_obj_uint64_ret q = {0}; remote(39, &q); usleep(300); }
        wmtr_host_get_stats(H, &st1);
        wmtr_host_set_spin(H, 20000000);
        { struct unixcall_generic_obj_uint64_ret q = {0}; remote(39, &q); }
        wmtr_host_get_stats(H, &st1);
        /* a gap that really lasted longer than the budget (this thread descheduled on a loaded or
           virtualized machine, e.g. a CI runner) is allowed to end in a sleep */
        unsigned long long long_gaps = 0;
        for (int i = 0; i < 20; i++) {
          struct unixcall_generic_obj_uint64_ret q = {0}; remote(39, &q);
          uint64_t g0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
          usleep(300);
          if (clock_gettime_nsec_np(CLOCK_MONOTONIC) - g0 >= 20000000) long_gaps++;
        }
        wmtr_host_get_stats(H, &st2);
        CHECK(st1.idle_sleeps - st0.idle_sleeps >= 15, "no spin: %llu sleeps in 20 gaps", st1.idle_sleeps - st0.idle_sleeps);
        CHECK(long_gaps <= 5 && st2.idle_sleeps - st1.idle_sleeps <= 1 + long_gaps,
              "20 ms spin: %llu sleeps in 20 gaps of 0.3 ms (%llu gaps lasted >= 20 ms)",
              st2.idle_sleeps - st1.idle_sleeps, long_gaps);
        wmtr_host_set_spin(H, WMTR_HOST_SPIN_US_DEFAULT * 1000);
      }
      wmtr_host_destroy(H);
      CHECK(n_released == 2 && released[1] == v2.ret_view, "destroy did not release the leftover view (%d releases)", n_released);
      /* WMTR_CALLSTATS: destroy wrote a snapshot; call 39 (sync, above) is in it with its sync count */
      FILE *sf = fopen(stats_path, "r");
      char sl[256]; int found = 0;
      while (sf && fgets(sl, sizeof sl, sf)) {
        unsigned long long t, n, sy, ns; int call; char name[96]; unsigned cls;
        if (sscanf(sl, "%llu,%d,%95[^,],%u,%llu,%llu,%llu", &t, &call, name, &cls, &n, &sy, &ns) == 7 && call == 39 && n >= 1 && sy >= 1)
          found = 1;
      }
      if (sf) fclose(sf);
      CHECK(found, "WMTR_CALLSTATS: no snapshot line for call 39");
      sf = fopen(stats_path, "r"); found = 0;
      while (sf && fgets(sl, sizeof sl, sf)) if (strstr(sl, ",200,_executor_sleeps,9,")) found = 1;
      if (sf) fclose(sf);
      CHECK(found, "WMTR_CALLSTATS: no executor sleep row");
      unlink(stats_path);
      unlink(trigger_path);
    }
  }
  printf(failures ? "wmtr_loopback: %d FAILED\n" : "wmtr_loopback: all passed\n", failures);
  return failures != 0;
}
