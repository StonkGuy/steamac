/* wmtr host executor. Consumes the request ring in order on one thread; calls that may block or
 * compile shaders/pipelines run on dispatch queues (their callers wait for the reply, so program
 * order between client threads is preserved: anything that depends on such a call is enqueued only
 * after its reply). The Metal work itself is DXMT's winemetal_unix.c (wmtr_impl_funcs), unchanged,
 * except for the calls overridden here (memory, presentation, events, paths). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <time.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define WINEMETAL_API
#include "winemetal_thunks.h"
#include "airconv_thunks.h"
#include "wmtr_wire.h"
#include "wmtr_host.h"

typedef int NTSTATUS;
typedef NTSTATUS (*impl_fn)(void *);
extern const void *wmtr_impl_funcs[];

#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)

#define WMTR_CLASS_A 0
#define WMTR_CLASS_S 1
#define WMTR_CLASS_B 2
#define WMTR_CLASS_C 3
#define WMTR_CLASS_N 4
static const uint8_t call_class[146] = {
#define X(i, n, t, c) [i] = WMTR_CLASS_##c,
#include "wmtr_calls.inc"
#undef X
};
/* the params struct each call's implementation reads and writes (the client sends exactly this) */
static const uint32_t call_params_size[146] = {
#define X(i, n, t, c) [i] = sizeof(t),
#include "wmtr_calls.inc"
#undef X
};
/* zeroed bytes after every request copy: a WMTR_REF_EXTRA pointer is only checked to start inside the payload,
 * the struct the implementation reads there may run past its end */
#define WMTR_COPY_SLACK 4096u

struct wmtr_host {
  struct wmtr_ring *ring;
  uint8_t *data;
  struct wmtr_slot *slots;
  const struct wmtr_host_ops *ops;
  void *ctx;
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  int kicked, quit;
  int faulted; /* a call of this client faulted (see fault containment) */
  /* the last calls (index, slot, first params after the call), dumped when a call faults */
  struct { uint16_t call, slot; uint32_t st; uint64_t q[4]; } trace[128];
  _Atomic uint32_t trace_n;
  dispatch_queue_t compile_q, block_q;
  pthread_mutex_t free_lock, event_lock;
  /* views the client created and has not released: released on destroy, so a client that exits
   * without ReleaseMetalView (DXMT never calls it at process exit) does not leave its layer behind */
  pthread_mutex_t view_lock;
  uint64_t *views;
  uint32_t n_views, cap_views;
  _Atomic uint64_t n_req, n_sync, n_bytes, n_worker, n_sleep;
  /* client-assigned encoder handles (WMTR_F_CLIENT_HANDLE): entry (handle % WMTR_HANDLE_SLOTS) */
  struct wmtr_handle { _Atomic uint64_t id; _Atomic uint64_t obj; } *handles;
  /* how long the executor polls an empty ring before it sleeps, in mach ticks (WMTR_HOST_SPIN_US) */
  _Atomic uint64_t spin_ticks;
};

static int log_level = 1;
#define LOG(lvl, ...) do { if (log_level >= (lvl)) fprintf(stderr, "wmtr-host: " __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------------------------------- *
 * fault containment: the executor runs inside the VMM, and every handle in a request is a raw host
 * pointer the guest sent. A bad one (a client or DXMT bug) must cost that game, not the whole VM: a
 * SIGSEGV/SIGBUS raised while a call executes jumps back to execute(), which fails the call and every
 * later call of that client. Faults anywhere else go to the previously installed handler (or the
 * default action). Cost on the common path: one sigsetjmp per call. */
#ifndef STATUS_ACCESS_VIOLATION
#define STATUS_ACCESS_VIOLATION ((NTSTATUS)0xC0000005)
#endif
static __thread sigjmp_buf *fault_jmp;
static __thread void *fault_addr;
static struct sigaction prev_segv, prev_bus;

static void
fault_handler(int sig, siginfo_t *si, void *uc) {
  if (fault_jmp) { fault_addr = si ? si->si_addr : NULL; siglongjmp(*fault_jmp, sig); }
  struct sigaction *prev = sig == SIGBUS ? &prev_bus : &prev_segv;
  if (prev->sa_flags & SA_SIGINFO) { prev->sa_sigaction(sig, si, uc); return; }
  if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) { prev->sa_handler(sig); return; }
  sigaction(sig, prev, NULL); /* default action: returning re-executes the fault and dies */
}

static void
fault_install(void) {
  struct sigaction sa = {0};
  sa.sa_sigaction = fault_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &prev_segv);
  sigaction(SIGBUS, &sa, &prev_bus);
}

/* ------------------------------------------------------------------------------------------- *
 * per-frame export (WMTR_FRAMES=<path>): one line per presented frame, in the steamac launcher's
 * STEAMAC_PERF_FRAMES format (t_ns,flush_ms,shown_ms,copy_ms,upload_ms,vsyncs) so its frame_stats.py
 * reads it unchanged. t_ns = completion of the command buffer that presents the drawable, on the mach
 * clock (CLOCK_UPTIME_RAW, the base of CACurrentMediaTime and of Python's time.monotonic_ns on macOS);
 * the other columns are 0 (they describe the scanout copy path, which wmtr frames do not take). Off
 * (one getenv) unless set. */
static pthread_mutex_t frames_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *
frames_out(void) {
  static FILE *f;
  static int tried;
  if (!tried) {
    pthread_mutex_lock(&frames_lock);
    if (!tried) {
      const char *p = getenv("WMTR_FRAMES");
      if (p && *p) {
        f = fopen(p, "a");
        if (f) setvbuf(f, NULL, _IOLBF, 0);
      }
      __atomic_store_n(&tried, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&frames_lock);
  }
  return f;
}

/* per-call statistics (WMTR_CALLSTATS=<path>): cumulative count, synchronous count and host execution
 * time per call, written every 10 s (and when a client goes away) as CSV snapshots
 * t_ns,call,name,class,count,sync,exec_ns (plus rows 200/201: executor sleeps and kicks, in count);
 * consecutive snapshots give the per-frame call profile of a
 * running game (presentDrawable = calls 47/48) without touching the guest. Off (one getenv) unless set. */
static const char *const call_name[146] = {
#define X(i, n, t, c) [i] = #n,
#include "wmtr_calls.inc"
#undef X
};
static _Atomic uint64_t cs_count[146], cs_sync[146], cs_ns[146];
/* executor sleeps (ring empty after the spin) and kicks (a client woke it): rows 200/201, class 9 */
static _Atomic uint64_t cs_sleeps, cs_kicks;
static FILE *cs_file;
static pthread_mutex_t cs_lock = PTHREAD_MUTEX_INITIALIZER;

static void
callstats_dump(void) {
  pthread_mutex_lock(&cs_lock);
  if (!cs_file) { pthread_mutex_unlock(&cs_lock); return; }
  uint64_t t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  for (int i = 0; i < 146; i++) {
    uint64_t n = atomic_load(&cs_count[i]);
    if (!n) continue;
    fprintf(cs_file, "%llu,%d,%s,%u,%llu,%llu,%llu\n", (unsigned long long)t, i, call_name[i] ? call_name[i] : "?",
            call_class[i], (unsigned long long)n, (unsigned long long)atomic_load(&cs_sync[i]),
            (unsigned long long)atomic_load(&cs_ns[i]));
  }
  fprintf(cs_file, "%llu,200,_executor_sleeps,9,%llu,0,0\n%llu,201,_kicks,9,%llu,0,0\n", (unsigned long long)t,
          (unsigned long long)atomic_load(&cs_sleeps), (unsigned long long)t, (unsigned long long)atomic_load(&cs_kicks));
  fflush(cs_file);
  pthread_mutex_unlock(&cs_lock);
}

static int cs_state; /* 1 on, 2 off */

/* Turned on at launch (WMTR_CALLSTATS=<path>) or at runtime: while the trigger file exists (default
 * $TMPDIR/wmtr-callstats.on, WMTR_CALLSTATS_TRIGGER overrides) statistics are collected into the path written in
 * it (or <trigger>.csv); removing it writes a last snapshot and stops collecting. The trigger is checked every
 * 5 s (WMTR_CALLSTATS_POLL_MS) by one otherwise idle thread. */
static char cs_trigger[1024];
static int cs_from_env;

static void
callstats_open(const char *path) {
  pthread_mutex_lock(&cs_lock);
  if (!cs_file && path && *path) cs_file = fopen(path, "a");
  if (cs_file) __atomic_store_n(&cs_state, 1, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&cs_lock);
}

static void *
callstats_thread(void *arg) {
  (void)arg;
  pthread_setname_np("wmtr-callstats");
  const char *pe = getenv("WMTR_CALLSTATS_POLL_MS");
  useconds_t poll = (useconds_t)(pe && *pe ? atoi(pe) : 5000) * 1000;
  uint64_t last = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  for (;;) {
    usleep(poll);
    if (!cs_from_env && cs_trigger[0]) {
      int want = access(cs_trigger, F_OK) == 0;
      int on = __atomic_load_n(&cs_state, __ATOMIC_ACQUIRE) == 1;
      if (want && !on) {
        char path[1024] = {0};
        FILE *t = fopen(cs_trigger, "r");
        if (t) { if (!fgets(path, sizeof path, t)) path[0] = 0; fclose(t); }
        path[strcspn(path, "\r\n")] = 0;
        if (!path[0]) snprintf(path, sizeof path, "%s.csv", cs_trigger);
        callstats_open(path);
      } else if (!want && on) {
        callstats_dump();
        pthread_mutex_lock(&cs_lock);
        __atomic_store_n(&cs_state, 2, __ATOMIC_RELEASE);
        fclose(cs_file); cs_file = NULL;
        pthread_mutex_unlock(&cs_lock);
      }
    }
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - last >= 10000000000ull) { last = now; callstats_dump(); }
  }
  return NULL;
}

static void
callstats_init(void) {
  const char *p = getenv("WMTR_CALLSTATS");
  if (p && *p) { cs_from_env = 1; callstats_open(p); }
  const char *t = getenv("WMTR_CALLSTATS_TRIGGER");
  if (t && *t) snprintf(cs_trigger, sizeof cs_trigger, "%s", t);
  else {
    char tmp[512];
    if (confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp)) snprintf(cs_trigger, sizeof cs_trigger, "%swmtr-callstats.on", tmp);
  }
  if (__atomic_load_n(&cs_state, __ATOMIC_ACQUIRE) != 1) __atomic_store_n(&cs_state, 2, __ATOMIC_RELEASE);
  pthread_t th;
  pthread_create(&th, NULL, callstats_thread, NULL);
  pthread_detach(th);
}

static int
callstats_on(void) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  if (__builtin_expect(!__atomic_load_n(&cs_state, __ATOMIC_ACQUIRE), 0)) pthread_once(&once, callstats_init);
  return __atomic_load_n(&cs_state, __ATOMIC_RELAXED) == 1;
}

static inline void
callstats_add(uint16_t call, int sync, uint64_t ns) {
  if (call >= 146) return;
  atomic_fetch_add_explicit(&cs_count[call], 1, memory_order_relaxed);
  if (sync) atomic_fetch_add_explicit(&cs_sync[call], 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&cs_ns[call], ns, memory_order_relaxed);
}

/* The launcher's layer policy (VMView: game layer visibility, Metal HUD placement) acts on a layer's Metal
 * HUD only once libMTLHud attached to it, which happens when the layer's first drawable reached the screen.
 * So the view re-applies its policy when a game layer's 1st, 10th and 60th frames are done. */
static char policy_frames_key;
static int
layer_policy_due(CALayer *layer) {
  NSNumber *n = objc_getAssociatedObject(layer, &policy_frames_key);
  int c = n.intValue + 1;
  if (c > 60) return 0;
  objc_setAssociatedObject(layer, &policy_frames_key, @(c), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  return c == 1 || c == 10 || c == 60;
}

static void
layer_policy_on_present(id<MTLCommandBuffer> cb, id drawable) {
  if (![drawable respondsToSelector:@selector(layer)]) return;
  CALayer *layer = [drawable layer];
  if (!layer || !layer_policy_due(layer)) return;
  [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
    (void)b;
    dispatch_async(dispatch_get_main_queue(), ^{
      id view = layer.superlayer.delegate;
      if ([view respondsToSelector:@selector(gameLayersVisible)])
        [view setValue:[view valueForKey:@"gameLayersVisible"] forKey:@"gameLayersVisible"];
    });
  }];
}

/* test hook: how many of `frames` presents of `layer` re-apply the view's policy */
__attribute__((visibility("default"))) int
wmtr_debug_layer_policy_frames(void *layer, int frames) {
  int n = 0;
  for (int i = 0; i < frames; i++) n += layer_policy_due((__bridge CALayer *)layer);
  return n;
}

static void
frame_record_on_completion(id<MTLCommandBuffer> cb) {
  [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
    (void)b;
    uint64_t t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&frames_lock);
    fprintf(frames_out(), "%llu,0,0,0,0,0\n", (unsigned long long)t);
    pthread_mutex_unlock(&frames_lock);
  }];
}

/* ------------------------------------------------------------------------------------------- */
static void
view_track(struct wmtr_host *h, uint64_t v) {
  pthread_mutex_lock(&h->view_lock);
  if (h->n_views == h->cap_views) {
    uint32_t cap = h->cap_views ? h->cap_views * 2 : 4;
    uint64_t *n = realloc(h->views, cap * sizeof *n);
    if (n) { h->views = n; h->cap_views = cap; }
  }
  if (h->n_views < h->cap_views) h->views[h->n_views++] = v;
  pthread_mutex_unlock(&h->view_lock);
}

/* 1 if `v` was a live view of this client (then it is no longer tracked) */
static int
view_untrack(struct wmtr_host *h, uint64_t v) {
  int found = 0;
  pthread_mutex_lock(&h->view_lock);
  for (uint32_t i = 0; i < h->n_views; i++)
    if (h->views[i] == v) { h->views[i] = h->views[--h->n_views]; found = 1; break; }
  pthread_mutex_unlock(&h->view_lock);
  return found;
}

static void *
resolve(struct wmtr_host *h, uint64_t v, uint8_t *payload, uint32_t payload_size) {
  uint64_t tag = v & WMTR_REF_MASK;
  if (tag == WMTR_REF_EXTRA) {
    uint32_t off = (uint32_t)v;
    return off < payload_size ? payload + off : NULL;
  }
  if (tag == WMTR_REF_ARENA) {
    uint32_t blob = (uint32_t)((v >> 32) & 0xffff), off = (uint32_t)v;
    uint64_t size = 0;
    uint8_t *b = h->ops->blob_ptr(h->ctx, blob, &size);
    return (b && off < size) ? b + off : NULL;
  }
  return NULL;
}

/* payload = NULL: the request's sizes could not be trusted (malformed request): status only */
static void
reply(struct wmtr_host *h, const struct wmtr_req_ext *r, const struct wmtr_out *outs, uint32_t n_out, uint8_t *payload,
      NTSTATUS st) {
  if (!(r->h.flags & WMTR_F_SYNC) || r->h.slot >= WMTR_SLOTS) return;
  struct wmtr_slot *s = &h->slots[r->h.slot];
  /* every size here is the client's: 64-bit sums, bounded by the slot and by the payload */
  uint64_t o = r->h.params_size;
  if (!payload || o > WMTR_SLOT_BYTES || o > r->payload_size) { o = 0; n_out = 0; if (payload) st = STATUS_INVALID_PARAMETER; }
  if (o) memcpy(s->data, payload, o);
  o = WMTR_ALIGN8(o);
  for (uint32_t i = 0; i < n_out; i++) {
    if (!outs[i].size) continue; /* arena output: already in shared memory */
    if (o + outs[i].size > WMTR_SLOT_BYTES || (uint64_t)outs[i].off + outs[i].size > r->payload_size) break;
    memcpy(s->data + o, payload + outs[i].off, outs[i].size);
    o += WMTR_ALIGN8((uint64_t)outs[i].size);
  }
  s->status = (uint32_t)st;
  s->out_size = (uint32_t)o;
  __atomic_store_n(&s->done_seq, r->h.seq, __ATOMIC_RELEASE);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (__atomic_load_n(&s->waiting, __ATOMIC_SEQ_CST)) h->ops->wake(h->ctx, r->h.slot);
}

/* ------------------------------------------------------------------------------------------- *
 * virtual autorelease pools. Natively each Wine thread runs its unix calls on its own OS thread and
 * DXMT brackets work in NSAutoreleasePools, relying on them to keep autoreleased results alive. Here
 * all client threads share host threads, so real pools would interleave. Instead each client thread
 * (reply slot) has a stack of virtual pools; results that DXMT's implementation returns autoreleased
 * are retained into the slot's current virtual pool and released when DXMT releases that pool. */
struct vpool {
  struct vpool *parent;
  uint64_t cid; /* the client's handle for it (WMTR_HANDLE_POOL), 0 if the host's address is the handle */
  uint32_t slot, used;
  id *objs;
  uint32_t n, cap;
};
#define VPOOLS 4096
static struct vpool vpools[VPOOLS];
static pthread_mutex_t vpool_lock = PTHREAD_MUTEX_INITIALIZER;
static struct vpool *vpool_top[WMTR_SLOTS];
#define VPOOL_LOOSE 256
static id loose[WMTR_SLOTS][VPOOL_LOOSE];
static uint32_t loose_next[WMTR_SLOTS];

static int
is_vpool(uint64_t h) {
  return h >= (uint64_t)(uintptr_t)&vpools[0] && h < (uint64_t)(uintptr_t)&vpools[VPOOLS] &&
         (h - (uint64_t)(uintptr_t)&vpools[0]) % sizeof(struct vpool) == 0;
}

static uint64_t
vpool_push(uint32_t slot, uint64_t cid) {
  pthread_mutex_lock(&vpool_lock);
  struct vpool *p = NULL;
  for (int i = 0; i < VPOOLS; i++) if (!vpools[i].used) { p = &vpools[i]; break; }
  if (p) {
    p->used = 1; p->slot = slot; p->n = 0; p->cid = cid;
    p->parent = slot < WMTR_SLOTS ? vpool_top[slot] : NULL;
    if (slot < WMTR_SLOTS) vpool_top[slot] = p;
  }
  pthread_mutex_unlock(&vpool_lock);
  return (uint64_t)(uintptr_t)p;
}

static void
vpool_pop(struct vpool *target) {
  pthread_mutex_lock(&vpool_lock);
  uint32_t slot = target->slot;
  struct vpool *p = slot < WMTR_SLOTS ? vpool_top[slot] : target;
  /* release `target` and every pool pushed after it (released out of order = drained with it) */
  while (p) {
    struct vpool *parent = p->parent;
    for (uint32_t i = 0; i < p->n; i++) [p->objs[i] release];
    p->n = 0; p->used = 0;
    if (p == target) { p = parent; break; }
    p = parent;
  }
  if (slot < WMTR_SLOTS) vpool_top[slot] = p;
  pthread_mutex_unlock(&vpool_lock);
}

/* the open pool of the client handle `cid`, 0 if there is none */
static uint64_t
vpool_find(uint64_t cid) {
  uint32_t slot = (uint32_t)(cid >> 32) & 0x7fff;
  uint64_t r = 0;
  if (slot >= WMTR_SLOTS) return 0;
  pthread_mutex_lock(&vpool_lock);
  for (struct vpool *p = vpool_top[slot]; p; p = p->parent)
    if (p->cid == cid) { r = (uint64_t)(uintptr_t)p; break; }
  pthread_mutex_unlock(&vpool_lock);
  return r;
}

static void
vpool_keep(uint32_t slot, uint64_t obj) {
  if (!obj || slot >= WMTR_SLOTS) return;
  pthread_mutex_lock(&vpool_lock);
  struct vpool *p = vpool_top[slot];
  if (p) {
    if (p->n == p->cap) { p->cap = p->cap ? p->cap * 2 : 64; p->objs = realloc(p->objs, p->cap * sizeof(id)); }
    p->objs[p->n++] = [(id)obj retain];
  } else {
    /* no pool open on the client thread: natively the object would live in the thread's outer pool
     * (e.g. an NSError whose description DXMT logs right after a failed PSO); our per-request pool
     * would free it at once. Keep the last VPOOL_LOOSE such results per slot alive instead. */
    id *ring = loose[slot];
    uint32_t i = loose_next[slot]++ % VPOOL_LOOSE;
    if (ring[i]) [ring[i] release];
    ring[i] = [(id)obj retain];
  }
  pthread_mutex_unlock(&vpool_lock);
}

/* results returned autoreleased by DXMT's winemetal_unix.c; +1 (new/alloc/copy) results and non-object
 * handles are not listed. Out-errors (ret_error) are not either: the pinned DXMT returns them retained
 * (utmapp/dxmt cc080d00), owned by the caller's WMT::Reference<WMT::Error> */
static void
keep_autoreleased(uint32_t slot, uint16_t call, uint8_t *params) {
  switch (call) {
  case 2: case 7: case 11: case 28: case 30: case 31: case 32: case 53: case 60: case 62: case 66: case 67:
  case 89: case 90: case 129: {
    /* `ret` is the last obj_handle_t of these generic structs; locate it by call */
    uint64_t ret = 0;
    switch (call) {
    case 2: case 31: case 32: ret = ((struct unixcall_generic_obj_uint64_obj_ret *)params)->ret; break;
    case 53: case 62: ret = ((struct unixcall_generic_obj_ret *)params)->ret; break;
    case 60: ret = ((struct unixcall_nsstring_string *)params)->ret; break;
    case 129: ret = ((struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers *)params)->ret; break;
    default: ret = ((struct unixcall_generic_obj_obj_ret *)params)->ret; break;
    }
    vpool_keep(slot, ret);
    break;
  }
  default: break;
  }
}

/* ------------------------------------------------------------------------------------------- *
 * overridden calls */
static void
push_free(struct wmtr_host *h, uint32_t blob, uint32_t off, uint64_t size) {
  pthread_mutex_lock(&h->free_lock);
  struct wmtr_ring *R = h->ring;
  uint64_t head = R->free_head;
  if (head - __atomic_load_n(&R->free_tail, __ATOMIC_ACQUIRE) >= WMTR_FREE_RING) {
    /* Full. The client drains the ring only from its own calls, and the call releasing this buffer may be the
     * one it is blocked in (quitting or logging out frees thousands of buffers in a row), so waiting here can
     * deadlock the game. Do not wait: this range is not handed back and stays the client's until it exits. */
    static _Atomic uint64_t dropped;
    uint64_t n = atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
    if (!(n % WMTR_FREE_RING)) LOG(0, "free ring full: %llu frees not returned\n", (unsigned long long)n + 1);
    pthread_mutex_unlock(&h->free_lock);
    return;
  }
  R->free_ring[head % WMTR_FREE_RING] = (struct wmtr_free_entry){blob, off, (uint32_t)size, 0};
  __atomic_store_n(&R->free_head, head + 1, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&h->free_lock);
}

static NTSTATUS
host_new_buffer(struct wmtr_host *h, struct unixcall_mtldevice_newbuffer *p, uint64_t arena_ref) {
  id<MTLDevice> device = (id<MTLDevice>)p->device;
  struct WMTBufferInfo *info = p->info.ptr;
  id<MTLBuffer> buffer;
  if (!info) return STATUS_INVALID_PARAMETER;
  if (info->memory.ptr) {
    /* client memory: the buffer must lie inside the arena blob the reference names (page-aligned, whole pages),
     * or the GPU and the NoCopy buffer would reach host memory past it */
    uint32_t blob = (uint32_t)((arena_ref >> 32) & 0xffff), off = (uint32_t)arena_ref;
    uint64_t blob_size = 0;
    uint8_t *base = arena_ref ? h->ops->blob_ptr(h->ctx, blob, &blob_size) : NULL;
    if (info->length > blob_size) return STATUS_INVALID_PARAMETER;
    uint64_t len = (info->length + WMTR_PAGE - 1) & ~(uint64_t)(WMTR_PAGE - 1);
    if (!base || info->memory.ptr != base + off || (off & (WMTR_PAGE - 1)) || !len || len > blob_size - off) {
      LOG(0, "newBuffer: memory outside its arena blob (blob %u off %u len %llu)\n", blob, off, (unsigned long long)len);
      return STATUS_INVALID_PARAMETER;
    }
    buffer = [device newBufferWithBytesNoCopy:info->memory.ptr
                                       length:len
                                      options:(MTLResourceOptions)info->options
                                  deallocator:^(void *pointer, NSUInteger length) {
                                    push_free(h, blob, off, length);
                                  }];
  } else {
    buffer = [device newBufferWithLength:info->length options:(MTLResourceOptions)info->options];
  }
  info->memory.ptr = NULL; /* the client keeps its own mapping */
  p->ret = (obj_handle_t)buffer;
  info->gpu_address = buffer ? [buffer gpuAddress] : 0;
  return buffer ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

struct host_listener { MTLSharedEventListener *listener; };

static void
push_event(struct wmtr_host *h, uint64_t listener, uint64_t event_handle) {
  pthread_mutex_lock(&h->event_lock);
  struct wmtr_ring *R = h->ring;
  uint64_t head = R->event_head;
  if (head - __atomic_load_n(&R->event_tail, __ATOMIC_ACQUIRE) < WMTR_EVENT_RING) {
    R->event_ring[head % WMTR_EVENT_RING] = (struct wmtr_event){listener, event_handle};
    __atomic_store_n(&R->event_head, head + 1, __ATOMIC_RELEASE);
  } else LOG(0, "event ring full, dropping\n");
  pthread_mutex_unlock(&h->event_lock);
  h->ops->wake(h->ctx, WMTR_EVENT_SLOT);
}

static NTSTATUS
host_call(struct wmtr_host *h, uint16_t call, uint8_t *params, const struct wmtr_req_ext *r, const uint32_t *relocs,
          const uint64_t *orig) {
  switch (call) {
  case 0: /* NSObject_retain */
    if (is_vpool(*(uint64_t *)params)) return STATUS_SUCCESS;
    break;
  case 1: /* NSObject_release: a virtual pool drains */
    if (is_vpool(*(uint64_t *)params)) { vpool_pop((struct vpool *)(uintptr_t)*(uint64_t *)params); return STATUS_SUCCESS; }
    break;
  case 10: /* NSAutoreleasePool_alloc_init */
    ((struct unixcall_generic_obj_ret *)params)->ret =
        vpool_push(r->h.slot, r->h.flags & WMTR_F_CLIENT_HANDLE ? ((struct unixcall_generic_obj_ret *)params)->ret : 0);
    return ((struct unixcall_generic_obj_ret *)params)->ret ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
  case 18: { /* newBuffer: memory field relocation carries the arena reference */
    uint64_t arena_ref = 0;
    struct unixcall_mtldevice_newbuffer *p = (void *)params;
    uint32_t mem_field = 0;
    for (int i = 0; i < r->n_reloc; i++)
      if ((orig[i] & WMTR_REF_MASK) == WMTR_REF_ARENA) { arena_ref = orig[i]; mem_field = relocs[i]; }
    (void)mem_field;
    return host_new_buffer(h, p, arena_ref);
  }
  case 72: { /* CreateMetalViewFromHWND */
    struct unixcall_create_metal_view_from_hwnd *p = (void *)params;
    if (!h->ops->create_view) return STATUS_UNSUCCESSFUL;
    uint64_t layer = 0;
    p->ret_view = h->ops->create_view(h->ctx, p->hwnd, (void *)p->device, &layer);
    p->ret_layer = layer;
    if (p->ret_view) view_track(h, p->ret_view);
    return p->ret_view ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
  }
  case 73: {
    uint64_t v = ((struct unixcall_generic_obj_noret *)params)->handle;
    if (view_untrack(h, v) && h->ops->release_view) h->ops->release_view(h->ctx, v);
    return STATUS_SUCCESS;
  }
  case 104: { /* setWin32EventAtValue: queue (listener, handle) for the client to NtSetEvent */
    struct unixcall_mtlsharedevent_setevent *p = (void *)params;
    struct host_listener *l = (struct host_listener *)p->shared_event_listener;
    uint64_t lid = p->shared_event_listener, ev = p->event_handle;
    if (!l) return STATUS_INVALID_PARAMETER;
    [(id<MTLSharedEvent>)p->shared_event notifyListener:l->listener
                                                atValue:p->value
                                                  block:^(id<MTLSharedEvent> e, uint64_t v) {
                                                    push_event(h, lid, ev);
                                                  }];
    return STATUS_SUCCESS;
  }
  case 108: {
    struct host_listener *l = calloc(1, sizeof *l);
    l->listener = [[MTLSharedEventListener alloc] init];
    ((struct unixcall_generic_obj_ret *)params)->ret = (obj_handle_t)l;
    return STATUS_SUCCESS;
  }
  case 110: { /* destroy: tell the client's listener thread to return; the listener object is kept
                 (late notifications for it are harmless and are dropped by the client) */
    uint64_t lid = ((struct unixcall_generic_obj_noret *)params)->handle;
    if (lid) push_event(h, lid, 0);
    return STATUS_SUCCESS;
  }
  case 112: case 113: case 115: case 117: case 119: if (h->ops->map_path) {
    /* the path is the first relocated field of these calls */
    for (int i = 0; i < r->n_reloc; i++) {
      if (relocs[i] >= r->h.params_size) continue;
      char **field = (char **)(params + relocs[i]);
      if (!*field) continue;
      char *buf = malloc(1024);
      const char *m = h->ops->map_path(h->ctx, *field, buf, 1024);
      if (m) *field = (char *)m;
      NTSTATUS st = ((impl_fn)wmtr_impl_funcs[call])(params);
      keep_autoreleased(r->h.slot, call, params);
      free(buf);
      return st;
    }
  }
  /* fallthrough */
  default:
    break;
  }
  impl_fn f = (impl_fn)wmtr_impl_funcs[call];
  if (!f) return STATUS_UNSUCCESSFUL;
  if (call == 47 || call == 48) { /* presentDrawable (both param structs start with buffer, drawable) */
    id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)((struct unixcall_generic_obj_obj_noret *)params)->handle;
    if (frames_out()) frame_record_on_completion(cb); /* record when the frame is done */
    layer_policy_on_present(cb, (id)((struct unixcall_generic_obj_obj_noret *)params)->arg);
  }
  NTSTATUS st = f(params);
  keep_autoreleased(r->h.slot, call, params);
  return st;
}

/* ------------------------------------------------------------------------------------------- *
 * client-assigned handles (WMTR_F_CLIENT_HANDLE, wmtr_proto.h). Cost for other calls: one compare of the
 * first params word. */
#ifndef STATUS_INVALID_HANDLE
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008)
#endif

/* offset of `ret` in the params of a call that may carry a client handle */
static int
handle_ret_off(uint16_t call) {
  switch (call) {
  case 10: return offsetof(struct unixcall_generic_obj_ret, ret);
  case 30: return offsetof(struct unixcall_generic_obj_obj_ret, ret);
  case 31: case 32: return offsetof(struct unixcall_generic_obj_uint64_obj_ret, ret);
  default: return -1;
  }
}

/* before the call: note the handle a creation carries (*cid), and give a call whose receiver is a client
 * handle the object (*w0 keeps the handle for the reply). 0: the handle names no object (logged); fail it. */
static int
handles_in(struct wmtr_host *h, const struct wmtr_req_ext *r, uint8_t *payload, uint64_t *w0, uint64_t *cid) {
  if (r->h.flags & WMTR_F_CLIENT_HANDLE) {
    int o = handle_ret_off(r->h.call);
    if (o >= 0 && o + 8 <= (int)r->h.params_size) memcpy(cid, payload + o, 8);
    if ((*cid & WMTR_REF_MASK) != WMTR_HANDLE_TAG) {
      LOG(0, "call %u: bad client handle %llx\n", r->h.call, (unsigned long long)*cid);
      *cid = 0;
      return 0;
    }
    if (o == 0) return 1; /* NSAutoreleasePool_alloc_init: the first word is the new handle */
  }
  if (r->h.params_size < 8) return 1;
  uint64_t v;
  memcpy(&v, payload, 8);
  if ((v & WMTR_REF_MASK) != WMTR_HANDLE_TAG) return 1;
  uint64_t obj = 0;
  if (v & WMTR_HANDLE_POOL) obj = vpool_find(v);
  else {
    struct wmtr_handle *e = &h->handles[v % WMTR_HANDLE_SLOTS];
    if (atomic_load_explicit(&e->id, memory_order_acquire) == v) obj = atomic_load_explicit(&e->obj, memory_order_relaxed);
  }
  if (!obj) {
    LOG(1, "call %u: client handle %llx names no object (its creation failed); call skipped\n", r->h.call,
        (unsigned long long)v);
    return 0;
  }
  *w0 = v;
  memcpy(payload, &obj, 8);
  return 1;
}

/* after a creation that carried client handle `cid` */
static void
handle_created(struct wmtr_host *h, const struct wmtr_req_ext *r, uint8_t *payload, uint64_t cid) {
  uint64_t obj;
  memcpy(&obj, payload + handle_ret_off(r->h.call), 8);
  if (!(cid & WMTR_HANDLE_POOL)) {
    struct wmtr_handle *e = &h->handles[cid % WMTR_HANDLE_SLOTS];
    atomic_store_explicit(&e->obj, obj, memory_order_relaxed);
    atomic_store_explicit(&e->id, cid, memory_order_release);
  }
  if (!obj) LOG(0, "call %s: creation of client handle %llx failed; the calls using it fail\n", call_name[r->h.call],
                (unsigned long long)cid);
}

/* ------------------------------------------------------------------------------------------- */
struct job { struct wmtr_host *h; uint8_t *copy; uint32_t len; };

static void
execute(struct wmtr_host *h, uint8_t *rec, uint32_t len) {
  const struct wmtr_req_ext *r = (const void *)rec;
  const uint32_t *relocs = (const void *)(rec + sizeof *r);
  const struct wmtr_out *outs = (const void *)(rec + sizeof *r + 4u * r->n_reloc);
  uint32_t poff = wmtr_req_payload_offset(r);
  uint8_t *payload = rec + poff;
  /* poff <= 16 + 4 * 65535 + 8 * 65535: no 32-bit overflow; the payload size is checked by subtraction */
  if (poff > len || r->payload_size > len - poff || r->h.params_size > r->payload_size) {
    LOG(0, "malformed request call=%u\n", r->h.call);
    reply(h, r, outs, 0, NULL, STATUS_INVALID_PARAMETER);
    return;
  }
  uint64_t orig_small[512];
  uint64_t *orig = r->n_reloc <= 512 ? orig_small : malloc(8u * r->n_reloc);
  int nr = r->n_reloc;
  NTSTATUS st = STATUS_SUCCESS;
  uint64_t w0 = 0, cid = 0;
  for (int i = 0; i < nr; i++) {
    if (r->payload_size < 8 || relocs[i] > r->payload_size - 8) { st = STATUS_INVALID_PARAMETER; break; }
    uint64_t v;
    memcpy(&v, payload + relocs[i], 8);
    orig[i] = v;
    void *p = v ? resolve(h, v, payload, r->payload_size) : NULL;
    if (v && !p) { st = STATUS_INVALID_PARAMETER; LOG(0, "call %u: bad reference %llx\n", r->h.call, (unsigned long long)v); break; }
    memcpy(payload + relocs[i], &p, 8);
  }
  if (st == STATUS_SUCCESS) {
    if (r->h.flags & WMTR_F_CONTROL) {
      st = STATUS_SUCCESS; /* PING and HELLO need no work */
    } else if (r->h.call < 146) {
      if (r->h.params_size < call_params_size[r->h.call]) {
        st = STATUS_INVALID_PARAMETER; /* the implementation would read and write past the params */
        LOG(0, "call %u: params %u bytes, expected %u\n", r->h.call, r->h.params_size, call_params_size[r->h.call]);
      } else if (__atomic_load_n(&h->faulted, __ATOMIC_ACQUIRE)) {
        st = STATUS_ACCESS_VIOLATION; /* this client already faulted the executor: refuse further work */
      } else if (!handles_in(h, r, payload, &w0, &cid)) {
        st = STATUS_INVALID_HANDLE; /* a client handle whose creation failed (logged) */
      } else {
        sigjmp_buf jb;
        if (sigsetjmp(jb, 1) == 0) {
          fault_jmp = &jb;
          int cs = callstats_on();
          uint64_t t0 = cs ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
          @autoreleasepool {
            st = host_call(h, r->h.call, payload, r, relocs, orig);
          }
          if (cs) callstats_add(r->h.call, r->h.flags & WMTR_F_SYNC, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
          fault_jmp = NULL;
          if (cid) handle_created(h, r, payload, cid);
          uint32_t ti = atomic_fetch_add(&h->trace_n, 1) % 128;
          h->trace[ti].call = r->h.call; h->trace[ti].slot = r->h.slot; h->trace[ti].st = (uint32_t)st;
          for (int k = 0; k < 4; k++) h->trace[ti].q[k] = r->h.params_size >= 8u * (k + 1) ? ((const uint64_t *)payload)[k] : 0;
        } else {
          fault_jmp = NULL;
          __atomic_store_n(&h->faulted, 1, __ATOMIC_RELEASE);
          const uint64_t *q = (const uint64_t *)payload;
          LOG(0, "call %u faulted at %p (bad handle from the client?); params %llx %llx %llx %llx; failing this "
                 "client's calls from now on\n", r->h.call, fault_addr, (unsigned long long)(r->h.params_size >= 8 ? q[0] : 0),
              (unsigned long long)(r->h.params_size >= 16 ? q[1] : 0), (unsigned long long)(r->h.params_size >= 24 ? q[2] : 0),
              (unsigned long long)(r->h.params_size >= 32 ? q[3] : 0));
          uint32_t tn = atomic_load(&h->trace_n);
          for (uint32_t k = tn > 128 ? tn - 128 : 0; k < tn; k++) {
            uint32_t ti = k % 128;
            LOG(0, "  before: call %u slot %u st %x: %llx %llx %llx %llx\n", h->trace[ti].call, h->trace[ti].slot, h->trace[ti].st,
                (unsigned long long)h->trace[ti].q[0], (unsigned long long)h->trace[ti].q[1],
                (unsigned long long)h->trace[ti].q[2], (unsigned long long)h->trace[ti].q[3]);
          }
          /* a by-pointer argument struct (e.g. a pipeline info) lives in the payload: show its head */
          if (r->h.params_size >= 16 && q[1] >= (uint64_t)(uintptr_t)payload && q[1] + 32 <= (uint64_t)(uintptr_t)payload + r->payload_size) {
            const uint64_t *in = (const uint64_t *)(uintptr_t)q[1];
            LOG(0, "  arg struct: %llx %llx %llx %llx\n", (unsigned long long)in[0], (unsigned long long)in[1],
                (unsigned long long)in[2], (unsigned long long)in[3]);
          }
          st = STATUS_ACCESS_VIOLATION;
        }
      }
    } else st = STATUS_INVALID_PARAMETER;
  }
  if (w0) memcpy(payload, &w0, 8); /* the caller gets its own handle back */
  reply(h, r, outs, r->n_out, payload, st);
  if (orig != orig_small) free(orig);
}

static void
job_run(void *arg) {
  struct job *j = arg;
  execute(j->h, j->copy, j->len);
  free(j->copy);
  free(j);
}

static void *
executor(void *arg) {
  struct wmtr_host *h = arg;
  struct wmtr_ring *R = h->ring;
  uint32_t scratch_cap = 1 << 20;
  uint8_t *scratch = calloc(1, scratch_cap + WMTR_COPY_SLACK);
  pthread_setname_np("wmtr-executor");
  for (;;) {
    uint64_t tail = R->tail;
    uint64_t head = __atomic_load_n(&R->head, __ATOMIC_ACQUIRE);
    if (tail == head) {
      /* poll for the spin budget, then sleep until kicked: a sleeping executor costs the next request a kick
       * (a virtio-gpu submission and a VM exit in the guest), a polling one costs host CPU */
      int got = 0;
      uint64_t end = mach_absolute_time() + atomic_load_explicit(&h->spin_ticks, memory_order_relaxed);
      do {
        if (__atomic_load_n(&R->head, __ATOMIC_ACQUIRE) != tail) { got = 1; break; }
        __asm__ __volatile__("yield");
      } while (mach_absolute_time() < end);
      if (got) continue;
      pthread_mutex_lock(&h->lock);
      __atomic_store_n(&R->host_idle, 1, __ATOMIC_SEQ_CST);
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
      while (!h->quit && !h->kicked && __atomic_load_n(&R->head, __ATOMIC_ACQUIRE) == tail) {
        atomic_fetch_add(&h->n_sleep, 1);
        if (cs_state == 1) atomic_fetch_add_explicit(&cs_sleeps, 1, memory_order_relaxed);
        pthread_cond_wait(&h->cond, &h->lock);
      }
      h->kicked = 0;
      __atomic_store_n(&R->host_idle, 0, __ATOMIC_SEQ_CST);
      int quit = h->quit;
      pthread_mutex_unlock(&h->lock);
      if (quit) break;
      continue;
    }
    uint32_t pos = (uint32_t)(tail % WMTR_RING_BYTES);
    uint32_t to_end = WMTR_RING_BYTES - pos;
    if (to_end < sizeof(struct wmtr_req_ext)) { __atomic_store_n(&R->tail, tail + to_end, __ATOMIC_RELEASE); continue; }
    struct wmtr_req hdr;
    memcpy(&hdr, h->data + pos, sizeof hdr);
    if (hdr.flags & WMTR_F_WRAP) { __atomic_store_n(&R->tail, tail + to_end, __ATOMIC_RELEASE); continue; }
    if (hdr.size < sizeof(struct wmtr_req_ext) || hdr.size > to_end || (hdr.size & 7)) {
      LOG(0, "ring corrupt at %llu (size %u); stopping\n", (unsigned long long)tail, hdr.size);
      break;
    }
    if (hdr.size > scratch_cap) { scratch_cap = hdr.size; scratch = realloc(scratch, scratch_cap + WMTR_COPY_SLACK); }
    memcpy(scratch, h->data + pos, hdr.size);
    memset(scratch + hdr.size, 0, WMTR_COPY_SLACK);
    __atomic_store_n(&R->tail, tail + hdr.size, __ATOMIC_RELEASE);
    atomic_fetch_add(&h->n_req, 1);
    atomic_fetch_add(&h->n_bytes, hdr.size);
    if (hdr.flags & WMTR_F_SYNC) atomic_fetch_add(&h->n_sync, 1);
    uint8_t cls = hdr.call < 146 && !(hdr.flags & WMTR_F_CONTROL) ? call_class[hdr.call] : WMTR_CLASS_S;
    if (cls == WMTR_CLASS_B || cls == WMTR_CLASS_C) {
      struct job *j = malloc(sizeof *j);
      j->h = h; j->len = hdr.size; j->copy = calloc(1, hdr.size + WMTR_COPY_SLACK);
      memcpy(j->copy, scratch, hdr.size);
      atomic_fetch_add(&h->n_worker, 1);
      dispatch_async_f(cls == WMTR_CLASS_B ? h->block_q : h->compile_q, j, job_run);
      continue;
    }
    execute(h, scratch, hdr.size);
  }
  free(scratch);
  return NULL;
}

struct wmtr_host *
wmtr_host_create(void *ring, const struct wmtr_host_ops *ops, void *ctx) {
  const char *l = getenv("WMTR_LOG");
  if (l) log_level = atoi(l);
  struct wmtr_ring *R = ring;
  /* the guest sizes blob 0: the executor reads the ring header, the request ring and the reply slots, and writes
   * the slots, all at fixed offsets up to WMTR_RING_BLOB_BYTES */
  uint64_t ring_size = 0;
  if (!ops->blob_ptr || ops->blob_ptr(ctx, 0, &ring_size) != ring || ring_size < WMTR_RING_BLOB_BYTES) {
    LOG(0, "ring blob of %llu bytes, need %llu\n", (unsigned long long)ring_size, (unsigned long long)WMTR_RING_BLOB_BYTES);
    return NULL;
  }
  if (R->magic != WMTR_RING_MAGIC || R->version != WMTR_VERSION) {
    LOG(0, "ring magic/version mismatch (%x/%u)\n", R->magic, R->version);
    return NULL;
  }
  static pthread_once_t fault_once = PTHREAD_ONCE_INIT;
  pthread_once(&fault_once, fault_install);
  struct wmtr_host *h = calloc(1, sizeof *h);
  h->ring = R;
  h->data = (uint8_t *)ring + WMTR_RING_DATA_OFFSET;
  h->slots = (struct wmtr_slot *)((uint8_t *)ring + WMTR_SLOTS_OFFSET);
  h->ops = ops; h->ctx = ctx;
  h->handles = calloc(WMTR_HANDLE_SLOTS, sizeof *h->handles);
  pthread_mutex_init(&h->lock, NULL); pthread_cond_init(&h->cond, NULL);
  const char *sp = getenv("WMTR_HOST_SPIN_US");
  wmtr_host_set_spin(h, (uint64_t)(sp && *sp ? atoll(sp) : WMTR_HOST_SPIN_US_DEFAULT) * 1000);
  pthread_mutex_init(&h->free_lock, NULL); pthread_mutex_init(&h->event_lock, NULL);
  pthread_mutex_init(&h->view_lock, NULL);
  h->compile_q = dispatch_queue_create("wmtr.compile", DISPATCH_QUEUE_CONCURRENT);
  h->block_q = dispatch_queue_create("wmtr.block", DISPATCH_QUEUE_CONCURRENT);
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
  pthread_create(&h->thread, &a, executor, h);
  return h;
}

void
wmtr_host_set_spin(struct wmtr_host *h, uint64_t ns) {
  static mach_timebase_info_data_t tb;
  if (!tb.denom) mach_timebase_info(&tb);
  atomic_store(&h->spin_ticks, ns * tb.denom / tb.numer);
}

void
wmtr_host_kick(struct wmtr_host *h) {
  if (cs_state == 1) atomic_fetch_add_explicit(&cs_kicks, 1, memory_order_relaxed);
  pthread_mutex_lock(&h->lock);
  h->kicked = 1;
  pthread_cond_signal(&h->cond);
  pthread_mutex_unlock(&h->lock);
}

void
wmtr_host_destroy(struct wmtr_host *h) {
  pthread_mutex_lock(&h->lock);
  h->quit = 1;
  pthread_cond_signal(&h->cond);
  pthread_mutex_unlock(&h->lock);
  pthread_join(h->thread, NULL);
  callstats_dump();
  /* let queued compile/blocking jobs finish: they call back into the embedder */
  dispatch_barrier_sync(h->compile_q, ^{});
  dispatch_barrier_sync(h->block_q, ^{});
  /* the client is gone: take down the views it never released */
  pthread_mutex_lock(&h->view_lock);
  for (uint32_t i = 0; i < h->n_views; i++)
    if (h->ops->release_view) h->ops->release_view(h->ctx, h->views[i]);
  h->n_views = 0;
  free(h->views); h->views = NULL; h->cap_views = 0;
  pthread_mutex_unlock(&h->view_lock);
}

uint64_t
wmtr_host_progress(struct wmtr_host *h, uint32_t slot) {
  if (slot < WMTR_SLOTS) return __atomic_load_n(&h->slots[slot].done_seq, __ATOMIC_ACQUIRE);
  return __atomic_load_n(&h->ring->event_head, __ATOMIC_ACQUIRE);
}

void
wmtr_host_get_stats(struct wmtr_host *h, struct wmtr_host_stats *o) {
  o->requests = h->n_req; o->sync_requests = h->n_sync; o->bytes = h->n_bytes;
  o->worker_calls = h->n_worker; o->idle_sleeps = h->n_sleep;
}

/* DXMT's unix side references ntdll's NtSetEvent for the CFRunLoop listener path, which the
 * host replaces (calls 104/108-110 above); this definition only satisfies the linker. */
NTSTATUS NtSetEvent(void *handle, void *prev) { (void)handle; (void)prev; return STATUS_UNSUCCESSFUL; }

/* ------------------------------------------------------------------------------------------- *
 * default presenter: one window per client HWND, created on the main thread of the embedding
 * application (wmtr-server, or the steamac launcher that hosts libkrun). It never activates, so it
 * does not take focus from the user's apps. */
/* WMTR_PRESENT=overlay (used inside the steamac launcher): the game's CAMetalLayer becomes an aspect-fit
 * sublayer of the launcher's VM view (class VMView, a layer-backed NSView whose backing layer shows the
 * guest scanout), so the game appears in the VM window and input keeps going to the guest unchanged.
 * Otherwise (wmtr-server): one window per HWND. */
static NSView *
find_vm_view(NSView *v) {
  if ([NSStringFromClass([v class]) hasSuffix:@"VMView"]) return v;
  for (NSView *c in [v subviews]) {
    NSView *f = find_vm_view(c);
    if (f) return f;
  }
  return nil;
}

__attribute__((visibility("default"))) uint64_t
wmtr_present_create_view(void *ctx, uint64_t hwnd, void *device, uint64_t *layer_out) {
  const char *mode = getenv("WMTR_PRESENT");
  int overlay = mode && !strcmp(mode, "overlay");
  __block NSView *view = nil;
  __block CAMetalLayer *layer = nil;
  dispatch_sync(dispatch_get_main_queue(), ^{
    layer = [CAMetalLayer layer];
    layer.device = (id<MTLDevice>)device;
    if (overlay) {
      for (NSWindow *w in [NSApp windows]) {
        NSView *vm = find_vm_view([w contentView]);
        if (!vm || !vm.layer) continue;
        view = [vm retain];
        layer.frame = vm.layer.bounds;
        layer.contentsScale = w.backingScaleFactor;
        layer.contentsGravity = kCAGravityResizeAspect;
        layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        layer.opaque = YES;
        /* drawables the game layer may have in flight: 2 (WMTR_DRAWABLES=3 restores Core Animation's default).
         * d3d11bench probe, DXMT, 60 Hz panel: app-reaction-to-present median 48.6 -> 41.3 ms, p90 52.1 -> 42.2 ms,
         * presented fps unchanged (119 vs 119) */
        const char *nd = getenv("WMTR_DRAWABLES");
        ((CAMetalLayer *)layer).maximumDrawableCount = nd && atoi(nd) == 3 ? 3 : 2;
        /* the launcher shows game layers only while a game has the guest's focus (VMView.gameLayersVisible) */
        if ([vm respondsToSelector:@selector(gameLayersVisible)])
          layer.hidden = ![[vm valueForKey:@"gameLayersVisible"] boolValue];
        [vm.layer addSublayer:layer];
        /* re-apply the view's layer policy (visibility, Metal HUD placement) now that the layer exists */
        if ([vm respondsToSelector:@selector(gameLayersVisible)])
          [vm setValue:[vm valueForKey:@"gameLayersVisible"] forKey:@"gameLayersVisible"];
        break;
      }
      if (!view) NSLog(@"wmtr: no VMView found for the overlay presenter");
      return;
    }
    NSRect r = NSMakeRect(80, 80, 1280, 800);
    NSWindow *w = [[NSWindow alloc] initWithContentRect:r
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    [w setTitle:[NSString stringWithFormat:@"wmtr hwnd %llx", (unsigned long long)hwnd]];
    view = [[NSView alloc] initWithFrame:r];
    view.wantsLayer = YES;
    layer.contentsScale = 1.0;
    view.layer = layer;
    [w setContentView:view];
    [w orderFront:nil]; /* never activate: do not take focus from the user's apps */
  });
  if (!view) return 0;
  *layer_out = (uint64_t)[layer retain];
  /* the handle released by ReleaseMetalView: remember the layer to detach it */
  objc_setAssociatedObject(view, (const void *)hwnd, layer, OBJC_ASSOCIATION_RETAIN);
  return (uint64_t)view;
}

__attribute__((visibility("default"))) void
wmtr_present_release_view(void *ctx, uint64_t v) {
  NSView *view = (NSView *)v;
  const char *mode = getenv("WMTR_PRESENT");
  int overlay = mode && !strcmp(mode, "overlay");
  dispatch_async(dispatch_get_main_queue(), ^{
    if (overlay) {
      for (CALayer *l in [[view.layer sublayers] copy])
        if ([l isKindOfClass:[CAMetalLayer class]] && l != view.layer) [l removeFromSuperlayer];
    } else {
      [[view window] close];
    }
    [view release];
  });
}

/* ring layout for embedders' tests (offsets within blob 0) */
__attribute__((visibility("default"))) void
wmtr_layout(uint64_t *ring_bytes, uint64_t *data_offset, uint64_t *slots_offset, uint64_t *slot_size) {
  *ring_bytes = WMTR_RING_BLOB_BYTES;
  *data_offset = WMTR_RING_DATA_OFFSET;
  *slots_offset = WMTR_SLOTS_OFFSET;
  *slot_size = sizeof(struct wmtr_slot);
}
