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
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define WINEMETAL_API
#include "winemetal_thunks.h"
#include "airconv_thunks.h"
#include "wmtr_wire.h"
#include "wmtr_handles.h"
#include "wmtr_host.h"
#include "wmtr_vr.h"

typedef int NTSTATUS;
typedef NTSTATUS (*impl_fn)(void *);
extern const void *wmtr_impl_funcs[];

#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)

#define WMTR_CLASS_A 0
#define WMTR_CLASS_S 1
#define WMTR_CLASS_B 2
#define WMTR_CLASS_C 3
#define WMTR_CLASS_N 4
static const uint8_t call_class[WMTR_NCALLS] = {
#define X(i, n, t, c) [i] = WMTR_CLASS_##c,
#include "wmtr_calls.inc"
#undef X
};
/* the params struct each call's implementation reads and writes (the client sends exactly this) */
static const uint32_t call_params_size[WMTR_NCALLS] = {
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
  _Atomic uint64_t n_req, n_sync, n_bytes, n_worker, n_sleep, n_refused;
  /* object handles (wmtr_handles.h) */
  struct wmtr_htab *htab;
  /* client-assigned encoder handles (WMTR_F_CLIENT_HANDLE): entry (handle % WMTR_HANDLE_SLOTS) holds the object's
   * table handle */
  struct wmtr_handle { _Atomic uint64_t id; _Atomic uint64_t obj; } *handles;
  /* how long the executor polls an empty ring before it sleeps, in mach ticks (WMTR_HOST_SPIN_US) */
  _Atomic uint64_t spin_ticks;
  /* VR export (call 146); owned when created from the environment */
  struct wmtr_vr *_Atomic vr;
  int owns_vr;
};

static int log_level = 1;
#define LOG(lvl, ...) do { if (log_level >= (lvl)) fprintf(stderr, "wmtr-host: " __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------------------------------- *
 * object handles (include/wmtr_handles.h): the per-context table every host object the guest refers to goes
 * through. Lookups are lock-free (an index into fixed chunks and one compare of the entry key); inserts, frees
 * and reference counts take the table lock. */
#define HCHUNK_BITS 12
#define HCHUNK (1u << HCHUNK_BITS)
#define HKEY_LIVE 0x100ull
#define HGEN_MASK 0xffffffull

struct hent {
  _Atomic uint64_t key; /* gen << 32 | HKEY_LIVE | kind; 0 bits 8: free */
  _Atomic uint64_t obj;
  uint64_t aux;         /* BITCODE_DATA: size; SM50_BITCODE: its BITCODE_DATA handle */
  uint32_t grefs;       /* references the guest owns (NSObject_release may drop that many) */
  uint32_t next_free;
};

struct wmtr_htab {
  _Atomic(struct hent *) chunks[WMTR_OBJ_MAX / HCHUNK];
  pthread_mutex_t lock;
  uint32_t n;                    /* entries ever used (index 0 is never a handle) */
  uint32_t free_head, free_tail, n_free; /* FIFO of freed entries: a slot is reused as late as possible */
  uint32_t *rev;                 /* object -> index (open addressing; 0 empty, UINT32_MAX deleted) */
  uint32_t rev_cap, rev_used;
  _Atomic uint32_t refs;         /* the context + one per live watcher */
  char assoc_key;                /* key of this table's watcher objects */
};

static struct hent *
hent_at(struct wmtr_htab *t, uint32_t i) {
  struct hent *c = atomic_load_explicit(&t->chunks[i >> HCHUNK_BITS], memory_order_acquire);
  return c ? &c[i & (HCHUNK - 1)] : NULL;
}

static struct wmtr_htab *
htab_create(void) {
  struct wmtr_htab *t = calloc(1, sizeof *t);
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&t->lock, &a);
  t->n = 1;
  t->refs = 1;
  return t;
}

static void
htab_unref(struct wmtr_htab *t) {
  if (atomic_fetch_sub(&t->refs, 1) != 1) return;
  for (uint32_t c = 0; c < WMTR_OBJ_MAX / HCHUNK; c++) free(atomic_load(&t->chunks[c]));
  free(t->rev);
  pthread_mutex_destroy(&t->lock);
  free(t);
}

static inline uint32_t
rev_hash(uint64_t obj, uint32_t cap) {
  obj ^= obj >> 33; obj *= 0xff51afd7ed558ccdull; obj ^= obj >> 33;
  return (uint32_t)obj & (cap - 1);
}

/* lock held */
static uint32_t
rev_find(struct wmtr_htab *t, uint64_t obj, uint32_t **slot_out) {
  if (!t->rev_cap) return 0;
  for (uint32_t i = rev_hash(obj, t->rev_cap), n = 0; n < t->rev_cap; i = (i + 1) & (t->rev_cap - 1), n++) {
    uint32_t v = t->rev[i];
    if (!v) return 0;
    if (v == UINT32_MAX) continue;
    struct hent *e = hent_at(t, v);
    if (atomic_load_explicit(&e->obj, memory_order_relaxed) == obj && (atomic_load_explicit(&e->key, memory_order_relaxed) & HKEY_LIVE)) {
      if (slot_out) *slot_out = &t->rev[i];
      return v;
    }
  }
  return 0;
}

static void
rev_add(struct wmtr_htab *t, uint64_t obj, uint32_t idx) {
  if ((t->rev_used + 1) * 2 > t->rev_cap) { /* rebuild at twice the live size (drops deleted marks) */
    uint32_t cap = t->rev_cap ? t->rev_cap : 1024;
    while (cap < (t->rev_used + 1) * 4) cap *= 2;
    uint32_t *old = t->rev, oc = t->rev_cap;
    t->rev = calloc(cap, sizeof *t->rev);
    t->rev_cap = cap;
    t->rev_used = 0;
    for (uint32_t i = 0; i < oc; i++)
      if (old[i] && old[i] != UINT32_MAX) {
        uint64_t o = atomic_load_explicit(&hent_at(t, old[i])->obj, memory_order_relaxed);
        uint32_t j = rev_hash(o, cap);
        while (t->rev[j]) j = (j + 1) & (cap - 1);
        t->rev[j] = old[i];
        t->rev_used++;
      }
    free(old);
  }
  uint32_t j = rev_hash(obj, t->rev_cap);
  while (t->rev[j] && t->rev[j] != UINT32_MAX) j = (j + 1) & (t->rev_cap - 1);
  if (!t->rev[j]) t->rev_used++;
  t->rev[j] = idx;
}

static inline uint64_t
hmake(uint32_t idx, uint64_t key) {
  return WMTR_OBJ_TAG | ((key >> 32) & HGEN_MASK) << WMTR_OBJ_INDEX_BITS | idx;
}

/* the entry a handle names, if it is live, of its generation and of a kind in `mask` */
static inline struct hent *
hentry(struct wmtr_htab *t, uint64_t h, uint64_t mask) {
  if ((h & WMTR_REF_MASK) != WMTR_OBJ_TAG) return NULL;
  uint32_t idx = (uint32_t)h & (WMTR_OBJ_MAX - 1);
  struct hent *e = hent_at(t, idx);
  if (!e) return NULL;
  uint64_t k = atomic_load_explicit(&e->key, memory_order_acquire);
  if (!(k & HKEY_LIVE) || ((k >> 32) & HGEN_MASK) != ((h >> WMTR_OBJ_INDEX_BITS) & HGEN_MASK) || !((mask >> (k & 0xff)) & 1))
    return NULL;
  return e;
}

static void hfree_idx(struct wmtr_htab *t, uint32_t idx, uint32_t gen);

/* frees its table entry when the object it is attached to is deallocated */
@interface WMTRHandleWatch : NSObject {
@public
  struct wmtr_htab *t;
  uint32_t idx, gen;
}
@end
@implementation WMTRHandleWatch
- (void)dealloc {
  hfree_idx(t, idx, gen);
  htab_unref(t);
  [super dealloc];
}
@end

static void
hfree_idx(struct wmtr_htab *t, uint32_t idx, uint32_t gen) {
  pthread_mutex_lock(&t->lock);
  struct hent *e = hent_at(t, idx);
  uint64_t k = e ? atomic_load_explicit(&e->key, memory_order_relaxed) : 0;
  if (e && (k & HKEY_LIVE) && ((k >> 32) & HGEN_MASK) == gen) {
    uint32_t *slot = NULL;
    if (rev_find(t, atomic_load_explicit(&e->obj, memory_order_relaxed), &slot) == idx) *slot = UINT32_MAX;
    atomic_store_explicit(&e->key, (uint64_t)((gen + 1) & HGEN_MASK) << 32, memory_order_release);
    e->grefs = 0;
    e->next_free = 0;
    if (t->free_tail) hent_at(t, t->free_tail)->next_free = idx;
    else t->free_head = idx;
    t->free_tail = idx;
    t->n_free++;
  }
  pthread_mutex_unlock(&t->lock);
}

/* the handle of an entry, freed */
static void
hfree(struct wmtr_htab *t, uint64_t h) {
  hfree_idx(t, (uint32_t)h & (WMTR_OBJ_MAX - 1), (uint32_t)(h >> WMTR_OBJ_INDEX_BITS) & HGEN_MASK);
}

static inline int
is_tagged_pointer(uint64_t p) {
  return (int64_t)p < 0; /* arm64 / x86-64 Objective-C tagged pointers: immortal, never deallocated */
}

/* the handle for `obj` (an existing entry's, else a new one of `kind`); `plus1`: the caller passes the guest one
 * reference it owns. 0 when the table is full. */
static uint64_t hinsert_w(struct wmtr_htab *t, uint64_t obj, int kind, int plus1, uint64_t aux, int watch);
static uint64_t
hinsert(struct wmtr_htab *t, uint64_t obj, int kind, int plus1, uint64_t aux) {
  return hinsert_w(t, obj, kind, plus1, aux, kind <= WMTR_K_LAST_OBJC);
}
static uint64_t
hinsert_w(struct wmtr_htab *t, uint64_t obj, int kind, int plus1, uint64_t aux, int watch) {
  if (!obj) return 0;
  pthread_mutex_lock(&t->lock);
  uint32_t idx = rev_find(t, obj, NULL);
  struct hent *e;
  if (idx) {
    e = hent_at(t, idx);
    e->grefs += plus1;
  } else {
    /* a freed entry is reused only behind 4096 others, so a stale handle meets a new generation late */
    if (t->n_free >= 4096 || (t->n >= WMTR_OBJ_MAX && t->free_head)) {
      idx = t->free_head;
      t->free_head = hent_at(t, idx)->next_free;
      if (!t->free_head) t->free_tail = 0;
      t->n_free--;
    } else if (t->n < WMTR_OBJ_MAX) {
      idx = t->n;
      if (!hent_at(t, idx)) atomic_store_explicit(&t->chunks[idx >> HCHUNK_BITS], calloc(HCHUNK, sizeof(struct hent)), memory_order_release);
      if (!hent_at(t, idx)) { pthread_mutex_unlock(&t->lock); return 0; }
      t->n++;
    } else {
      pthread_mutex_unlock(&t->lock);
      LOG(0, "handle table full\n");
      return 0;
    }
    e = hent_at(t, idx);
    uint64_t gen = atomic_load_explicit(&e->key, memory_order_relaxed) >> 32;
    atomic_store_explicit(&e->obj, obj, memory_order_relaxed);
    e->aux = aux;
    e->grefs = plus1;
    atomic_store_explicit(&e->key, gen << 32 | HKEY_LIVE | (uint64_t)kind, memory_order_release);
    rev_add(t, obj, idx);
    if (watch && !is_tagged_pointer(obj)) {
      WMTRHandleWatch *w = [WMTRHandleWatch alloc];
      w->t = t; w->idx = idx; w->gen = (uint32_t)gen & HGEN_MASK;
      atomic_fetch_add(&t->refs, 1);
      objc_setAssociatedObject((id)obj, &t->assoc_key, w, OBJC_ASSOCIATION_RETAIN);
      [w release];
    }
  }
  uint64_t h = hmake(idx, atomic_load_explicit(&e->key, memory_order_relaxed));
  pthread_mutex_unlock(&t->lock);
  return h;
}

/* the live entry holding `obj` of a kind in `mask`, freed (a destroyed non-Objective-C object) */
static void
hfree_obj(struct wmtr_htab *t, uint64_t obj, uint64_t mask) {
  pthread_mutex_lock(&t->lock);
  uint32_t idx = rev_find(t, obj, NULL);
  struct hent *e = idx ? hent_at(t, idx) : NULL;
  uint64_t k = e ? atomic_load_explicit(&e->key, memory_order_relaxed) : 0;
  if (e && ((mask >> (k & 0xff)) & 1)) hfree_idx(t, idx, (uint32_t)(k >> 32) & HGEN_MASK);
  pthread_mutex_unlock(&t->lock);
}

/* kind of an Objective-C object a DXMT call returned (once per new entry; classes are cached) */
static int
classify(uint64_t obj) {
  static struct { Class c; int k; } cache[512];
  static pthread_mutex_t cl = PTHREAD_MUTEX_INITIALIZER;
  Class c = object_getClass((id)obj);
  uint32_t i0 = (uint32_t)(((uintptr_t)c >> 4) * 2654435761u) & 511;
  pthread_mutex_lock(&cl);
  for (uint32_t n = 0, i = i0; n < 512; n++, i = (i + 1) & 511) {
    if (cache[i].c == c) { int k = cache[i].k; pthread_mutex_unlock(&cl); return k; }
    if (!cache[i].c) break;
  }
  pthread_mutex_unlock(&cl);
  static const struct { const char *proto; int kind; } protos[] = {
      {"MTLDevice", WMTR_K_DEVICE}, {"MTLCommandQueue", WMTR_K_QUEUE}, {"MTLCommandBuffer", WMTR_K_CMDBUF},
      {"MTLTexture", WMTR_K_TEXTURE}, {"MTLBuffer", WMTR_K_BUFFER}, {"MTLSamplerState", WMTR_K_SAMPLER},
      {"MTLDepthStencilState", WMTR_K_DSSO}, {"MTLLibrary", WMTR_K_LIBRARY}, {"MTLFunction", WMTR_K_FUNCTION},
      {"MTLComputePipelineState", WMTR_K_CPSO}, {"MTLRenderPipelineState", WMTR_K_RPSO},
      {"MTLBlitCommandEncoder", WMTR_K_ENC_BLIT}, {"MTLComputeCommandEncoder", WMTR_K_ENC_COMPUTE},
      {"MTLRenderCommandEncoder", WMTR_K_ENC_RENDER}, {"MTLSharedEvent", WMTR_K_SHARED_EVENT},
      {"MTLEvent", WMTR_K_EVENT}, {"MTLFence", WMTR_K_FENCE}, {"MTLBinaryArchive", WMTR_K_ARCHIVE},
      {"MTLHeap", WMTR_K_HEAP}, {"MTLIndirectCommandBuffer", WMTR_K_ICB}, {"MTLCounterSampleBuffer", WMTR_K_COUNTERBUF},
      {"MTLResidencySet", WMTR_K_RESSET}, {"MTLFXTemporalScaler", WMTR_K_FXT}, {"MTLFXSpatialScaler", WMTR_K_FXS},
      {"CAMetalDrawable", WMTR_K_DRAWABLE}, {"OS_dispatch_data", WMTR_K_DATA},
  };
  int k = WMTR_K_OBJC;
  for (size_t i = 0; i < sizeof protos / sizeof *protos && k == WMTR_K_OBJC; i++) {
    Protocol *p = objc_getProtocol(protos[i].proto);
    if (p && class_conformsToProtocol(c, p)) k = protos[i].kind;
    /* conformance is often declared on a superclass */
    for (Class s = class_getSuperclass(c); p && s && k == WMTR_K_OBJC; s = class_getSuperclass(s))
      if (class_conformsToProtocol(s, p)) k = protos[i].kind;
  }
  if (k == WMTR_K_OBJC) {
    if ([(id)obj isKindOfClass:[NSString class]]) k = WMTR_K_STRING;
    else if ([(id)obj isKindOfClass:[NSArray class]]) k = WMTR_K_ARRAY;
    else if ([(id)obj isKindOfClass:[NSError class]]) k = WMTR_K_ERROR;
    else if ([(id)obj isKindOfClass:[CAMetalLayer class]]) k = WMTR_K_LAYER;
    else if ([(id)obj isKindOfClass:[MTLCaptureManager class]]) k = WMTR_K_CAPTURE;
  }
  pthread_mutex_lock(&cl);
  for (uint32_t n = 0, i = i0; n < 512; n++, i = (i + 1) & 511)
    if (!cache[i].c || cache[i].c == c) { cache[i].c = c; cache[i].k = k; break; }
  pthread_mutex_unlock(&cl);
  return k;
}

/* ------------------------------------------------------------------------------------------- *
 * fault containment: the executor runs inside the VMM. Handles and pointers are checked (above), but a
 * fault in DXMT's implementation or Metal (a client or DXMT bug) must cost that game, not the whole VM: a
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
static const char *const call_name[WMTR_NCALLS] = {
#define X(i, n, t, c) [i] = #n,
#include "wmtr_calls.inc"
#undef X
};
static _Atomic uint64_t cs_count[WMTR_NCALLS], cs_sync[WMTR_NCALLS], cs_ns[WMTR_NCALLS];
/* executor sleeps (ring empty after the spin) and kicks (a client woke it): rows 200/201, class 9 */
static _Atomic uint64_t cs_sleeps, cs_kicks;
/* the process's VR link, for rows 210-213 (class 9): frames in count, the stage's cumulative time in exec_ns */
static _Atomic(struct wmtr_vr *) cs_vr;
static const char *const cs_vr_stage[WMTR_VR_STAGES] = {"_vr_submit_to_encoded", "_vr_encoded_to_rendered", "_vr_copy", "_vr_copied_to_sent"};
/* render-pass profile counters (pp_pass / pp_commit below) */
static _Atomic uint64_t pp_cb_count, pp_cb_gpu_ns, pp_passes, pp_load_bytes, pp_store_bytes, pp_resolve_bytes,
    pp_msaa_store_bytes, pp_msaa_passes;
#define PP_SIGS 512
static struct pp_sig { char name[96]; uint64_t count, bytes; } pp_sig[PP_SIGS];
static pthread_mutex_t pp_lock = PTHREAD_MUTEX_INITIALIZER;
/* the order of the last frame's encoders (row 250, one line): R<sig index> render pass, C compute, B blit, | present */
static char pp_seq[2][4096];
static int pp_seq_len, pp_seq_done; /* pp_seq[0] being filled, pp_seq[1] the last complete frame */
static FILE *cs_file;
static pthread_mutex_t cs_lock = PTHREAD_MUTEX_INITIALIZER;

static void
callstats_dump(void) {
  pthread_mutex_lock(&cs_lock);
  if (!cs_file) { pthread_mutex_unlock(&cs_lock); return; }
  uint64_t t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  for (int i = 0; i < WMTR_NCALLS; i++) {
    uint64_t n = atomic_load(&cs_count[i]);
    if (!n) continue;
    fprintf(cs_file, "%llu,%d,%s,%u,%llu,%llu,%llu\n", (unsigned long long)t, i, call_name[i] ? call_name[i] : "?",
            call_class[i], (unsigned long long)n, (unsigned long long)atomic_load(&cs_sync[i]),
            (unsigned long long)atomic_load(&cs_ns[i]));
  }
  fprintf(cs_file, "%llu,200,_executor_sleeps,9,%llu,0,0\n%llu,201,_kicks,9,%llu,0,0\n", (unsigned long long)t,
          (unsigned long long)atomic_load(&cs_sleeps), (unsigned long long)t, (unsigned long long)atomic_load(&cs_kicks));
  struct wmtr_vr *vr = atomic_load(&cs_vr);
  if (vr) {
    struct wmtr_vr_stats v;
    wmtr_vr_get_stats(vr, &v);
    for (int i = 0; i < WMTR_VR_STAGES; i++)
      fprintf(cs_file, "%llu,%d,%s,9,%llu,0,%llu\n", (unsigned long long)t, 210 + i, cs_vr_stage[i],
              (unsigned long long)v.stage_frames, (unsigned long long)v.stage_ns[i]);
    fprintf(cs_file, "%llu,214,_vr_dropped,9,%llu,0,0\n", (unsigned long long)t, (unsigned long long)v.dropped);
  }
  fprintf(cs_file, "%llu,220,_gpu_cmdbuf,9,%llu,0,%llu\n", (unsigned long long)t, (unsigned long long)atomic_load(&pp_cb_count),
          (unsigned long long)atomic_load(&pp_cb_gpu_ns));
  fprintf(cs_file, "%llu,221,_pass_load_bytes,9,%llu,0,%llu\n%llu,222,_pass_store_bytes,9,%llu,0,%llu\n"
                   "%llu,223,_pass_resolve_bytes,9,%llu,0,%llu\n%llu,224,_pass_msaa_store_bytes,9,%llu,0,%llu\n",
          (unsigned long long)t, (unsigned long long)atomic_load(&pp_passes), (unsigned long long)atomic_load(&pp_load_bytes),
          (unsigned long long)t, (unsigned long long)atomic_load(&pp_passes), (unsigned long long)atomic_load(&pp_store_bytes),
          (unsigned long long)t, (unsigned long long)atomic_load(&pp_passes), (unsigned long long)atomic_load(&pp_resolve_bytes),
          (unsigned long long)t, (unsigned long long)atomic_load(&pp_msaa_passes),
          (unsigned long long)atomic_load(&pp_msaa_store_bytes));
  pthread_mutex_lock(&pp_lock);
  for (int i = 0; i < PP_SIGS; i++)
    if (pp_sig[i].count)
      fprintf(cs_file, "%llu,%d,_sig %s,9,%llu,0,%llu\n", (unsigned long long)t, 230 + i, pp_sig[i].name,
              (unsigned long long)pp_sig[i].count, (unsigned long long)pp_sig[i].bytes);
  if (pp_seq_done) {
    for (char *c = pp_seq[1]; *c; c++) if (*c == ',') *c = ';';
    fprintf(cs_file, "%llu,250,_seq %s,9,1,0,0\n", (unsigned long long)t, pp_seq[1]);
  }
  pthread_mutex_unlock(&pp_lock);
  fflush(cs_file);
  pthread_mutex_unlock(&cs_lock);
}

/* the VR link the snapshots report: `now` replaces it, `was` is forgotten before its owner destroys it */
static void
callstats_vr(struct wmtr_vr *was, struct wmtr_vr *now) {
  pthread_mutex_lock(&cs_lock);
  if (now) atomic_store(&cs_vr, now);
  else if (was && atomic_load(&cs_vr) == was) atomic_store(&cs_vr, NULL);
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
  if (call >= WMTR_NCALLS) return;
  atomic_fetch_add_explicit(&cs_count[call], 1, memory_order_relaxed);
  if (sync) atomic_fetch_add_explicit(&cs_sync[call], 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&cs_ns[call], ns, memory_order_relaxed);
}

/* Render-pass profile (with the call statistics only, rows 220-239): GPU time of every committed command
 * buffer, and per render pass the attachments' estimated memory traffic (load / store / MSAA resolve bytes from
 * the attachment sizes and actions) plus the most expensive pass signatures. Shows what a frame's passes cost
 * on a tile GPU (stored MSAA attachments, redundant loads) without a frame capture of the game. */

static uint64_t
pp_tex_bytes(id<MTLTexture> t) {
  if (!t) return 0;
  NSUInteger bpp;
  switch (t.pixelFormat) {
  case MTLPixelFormatRGBA16Float: case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA16Unorm: bpp = 8; break;
  case MTLPixelFormatRGBA32Float: bpp = 16; break;
  case MTLPixelFormatR8Unorm: bpp = 1; break;
  case MTLPixelFormatRG8Unorm: case MTLPixelFormatR16Float: case MTLPixelFormatDepth16Unorm: bpp = 2; break;
  case MTLPixelFormatDepth32Float_Stencil8: bpp = 5; break;
  default: bpp = 4; break;
  }
  return (uint64_t)t.width * t.height * bpp * (t.sampleCount ? t.sampleCount : 1);
}

static void
pp_pass(const struct WMTRenderPassInfo *info) {
  uint64_t load = 0, store = 0, resolve = 0, msaa_store = 0;
  int msaa = 0;
  char sig[96];
  int n = 0;
  for (int i = 0; i < 8; i++) {
    id<MTLTexture> t = (id<MTLTexture>)info->colors[i].texture;
    if (!t) continue;
    uint64_t b = pp_tex_bytes(t);
    int st = info->colors[i].store_action;
    if (info->colors[i].load_action == WMTLoadActionLoad) load += b;
    if (st == WMTStoreActionStore || st == WMTStoreActionStoreAndMultisampleResolve) store += b;
    if (t.sampleCount > 1) {
      msaa = 1;
      if (st == WMTStoreActionStore || st == WMTStoreActionStoreAndMultisampleResolve) msaa_store += b;
    }
    if (info->colors[i].resolve_texture) resolve += pp_tex_bytes((id<MTLTexture>)info->colors[i].resolve_texture);
    if (n < 70)
      n += snprintf(sig + n, sizeof sig - n, "c%lux%lu/f%lu/s%lu/L%d/S%d ", (unsigned long)t.width, (unsigned long)t.height,
                    (unsigned long)t.pixelFormat, (unsigned long)t.sampleCount, info->colors[i].load_action, st);
  }
  id<MTLTexture> d = (id<MTLTexture>)info->depth.texture;
  if (d) {
    uint64_t b = pp_tex_bytes(d);
    if (info->depth.load_action == WMTLoadActionLoad) load += b;
    if (info->depth.store_action == WMTStoreActionStore) store += b;
    if (d.sampleCount > 1 && info->depth.store_action == WMTStoreActionStore) msaa_store += b;
    if (n < 80) n += snprintf(sig + n, sizeof sig - n, "d/s%lu/L%d/S%d", (unsigned long)d.sampleCount, info->depth.load_action,
                              info->depth.store_action);
  }
  if (!n) snprintf(sig, sizeof sig, "no attachments %ux%u", info->render_target_width, info->render_target_height);
  atomic_fetch_add_explicit(&pp_passes, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&pp_load_bytes, load, memory_order_relaxed);
  atomic_fetch_add_explicit(&pp_store_bytes, store, memory_order_relaxed);
  atomic_fetch_add_explicit(&pp_resolve_bytes, resolve, memory_order_relaxed);
  atomic_fetch_add_explicit(&pp_msaa_store_bytes, msaa_store, memory_order_relaxed);
  if (msaa) atomic_fetch_add_explicit(&pp_msaa_passes, 1, memory_order_relaxed);
  pthread_mutex_lock(&pp_lock);
  int slot = -1;
  for (int i = 0; i < PP_SIGS; i++) {
    if (!pp_sig[i].count) { if (slot < 0) slot = i; continue; }
    if (!strcmp(pp_sig[i].name, sig)) { slot = i; break; }
  }
  if (pp_seq_len < (int)sizeof pp_seq[0] - 8) pp_seq_len += snprintf(pp_seq[0] + pp_seq_len, sizeof pp_seq[0] - pp_seq_len, "R%d ", slot);
  if (slot >= 0) {
    if (!pp_sig[slot].count) snprintf(pp_sig[slot].name, sizeof pp_sig[slot].name, "%s", sig);
    pp_sig[slot].count++;
    pp_sig[slot].bytes += load + store + resolve;
  }
  pthread_mutex_unlock(&pp_lock);
}

static void
pp_event(const char *e) {
  pthread_mutex_lock(&pp_lock);
  if (e[0] == '|') {
    if (pp_seq_len) { memcpy(pp_seq[1], pp_seq[0], sizeof pp_seq[0]); pp_seq_done = 1; }
    pp_seq_len = 0;
    pp_seq[0][0] = 0;
  } else if (pp_seq_len < (int)sizeof pp_seq[0] - 8)
    pp_seq_len += snprintf(pp_seq[0] + pp_seq_len, sizeof pp_seq[0] - pp_seq_len, "%s ", e);
  pthread_mutex_unlock(&pp_lock);
}

static void
pp_commit(id<MTLCommandBuffer> cb) {
  [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
    double g = done.GPUEndTime - done.GPUStartTime;
    if (g > 0 && g < 10) atomic_fetch_add_explicit(&pp_cb_gpu_ns, (uint64_t)(g * 1e9), memory_order_relaxed);
    atomic_fetch_add_explicit(&pp_cb_count, 1, memory_order_relaxed);
  }];
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
  uint64_t cid; /* the client's handle for it (WMTR_HANDLE_POOL), 0 if it has a table handle */
  uint64_t hdl; /* its table handle (no client handle) */
  struct wmtr_htab *htab;
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

static uint64_t
vpool_push(uint32_t slot, uint64_t cid) {
  pthread_mutex_lock(&vpool_lock);
  struct vpool *p = NULL;
  for (int i = 0; i < VPOOLS; i++) if (!vpools[i].used) { p = &vpools[i]; break; }
  if (p) {
    p->used = 1; p->slot = slot; p->n = 0; p->cid = cid; p->hdl = 0; p->htab = NULL;
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
    if (p->hdl) { hfree(p->htab, p->hdl); htab_unref(p->htab); }
    p->hdl = 0; p->htab = NULL;
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
 * request translation: every handle field (src/wmtr_handles.inc) through the table, every pointer field checked
 * to be NULL or relocated, structs that carry handles checked to lie in the request's private copy. */
#include "wmtr_handles.inc"

static const struct wmtr_hfield cmd_nop_fields[] = {{offsetof(struct wmtcmd_base, next), WMTR_HF_PTR, 0, NULL}, {0, WMTR_HF_END, 0, NULL}};
static const struct wmtr_hcmd *cmd_index[3][256];
static uint8_t call_needs[WMTR_NCALLS]; /* 1: has pointer fields (needs the relocation map), 2: has results */
#define NEED_PTR 1
#define NEED_OUT 2

static uint8_t
fields_needs(const struct wmtr_hfield *f) {
  uint8_t n = 0;
  for (; f && f->flags != WMTR_HF_END; f++) {
    if (f->flags == WMTR_HF_OUT0 || f->flags == WMTR_HF_OUT1) n |= NEED_OUT;
    if (f->flags == WMTR_HF_PTR) {
      n |= NEED_PTR;
      if (f->sub) {
        if (f->sub->type == WMTR_HP_HANDLES_OUT0 || f->sub->type == WMTR_HP_HANDLE_OUT1) n |= NEED_OUT;
        n |= fields_needs(f->sub->fields);
      }
    }
  }
  return n;
}

static void
xlate_init(void) {
  for (size_t i = 0; i < sizeof wmtr_cmd_hfields / sizeof *wmtr_cmd_hfields; i++)
    if (wmtr_cmd_hfields[i].type < 256) cmd_index[wmtr_cmd_hfields[i].kind][wmtr_cmd_hfields[i].type] = &wmtr_cmd_hfields[i];
  for (int c = 0; c < WMTR_NCALLS; c++) call_needs[c] = fields_needs(wmtr_call_hfields[c]);
  call_needs[77] |= NEED_OUT;
}

struct xreq {
  struct wmtr_host *h;
  uint8_t *payload;
  uint32_t psize, params_size;
  const uint64_t *relmap; /* bit per 8-byte payload word that a relocation rewrote */
  uint16_t call;
  int save; /* sync: params-level handles are put back before the reply */
  uint32_t n_undo;
  struct { uint32_t off; uint64_t v; } undo[32];
  uint64_t bad; /* the refused value, for the log */
};

static inline int
x_inside(const struct xreq *x, const uint8_t *p, uint64_t size) {
  return p >= x->payload && size <= x->psize && (uint64_t)(p - x->payload) <= x->psize - size;
}

static inline int
x_relocated(const struct xreq *x, const uint8_t *field) {
  uint64_t off = (uint64_t)(field - x->payload);
  return x->relmap && !(off & 7) && off < x->psize && ((x->relmap[off >> 9] >> ((off >> 3) & 63)) & 1);
}

/* client-assigned handles (WMTR_F_CLIENT_HANDLE): entry (handle % WMTR_HANDLE_SLOTS) holds the table handle */
static uint64_t vpool_find(uint64_t cid);

/* the object a guest handle names, if it is of a kind in `mask`; 0 otherwise */
static inline uint64_t
resolve_handle(struct wmtr_host *h, uint64_t v, uint64_t mask) {
  if ((v & WMTR_REF_MASK) == WMTR_HANDLE_TAG) {
    if (v & WMTR_HANDLE_POOL) return (mask & WMTR_M_POOL) ? vpool_find(v) : 0;
    struct wmtr_handle *c = &h->handles[v % WMTR_HANDLE_SLOTS];
    if (atomic_load_explicit(&c->id, memory_order_acquire) != v) return 0;
    v = atomic_load_explicit(&c->obj, memory_order_relaxed);
  }
  struct hent *e = hentry(h->htab, v, mask);
  return e ? atomic_load_explicit(&e->obj, memory_order_relaxed) : 0;
}

static int x_in(struct xreq *x, const struct wmtr_hfield *f, uint8_t *base);

static uint64_t
x_count(const struct wmtr_hptee *s, const uint8_t *container) {
  uint64_t n = 0;
  switch (s->count_bytes) {
  case 0: return 1;
  case 1: return container[s->count_off];
  case 2: { uint16_t v; memcpy(&v, container + s->count_off, 2); return v; }
  case 4: { uint32_t v; memcpy(&v, container + s->count_off, 4); return v; }
  default: memcpy(&n, container + s->count_off, 8); return n;
  }
}

static int
x_cmdlist(struct xreq *x, uint8_t *node) {
  int kind = x->call - 36;
  if (kind < 0 || kind > 2) return 0;
  uint8_t *prev = NULL;
  while (node) {
    /* nodes are in the request in list order: no cycles, no shared nodes */
    if (node <= prev || !x_inside(x, node, sizeof(struct wmtcmd_base))) return 0;
    uint16_t type;
    memcpy(&type, node, 2);
    const struct wmtr_hfield *fields;
    uint32_t size;
    if (type == 0) { fields = cmd_nop_fields; size = sizeof(struct wmtcmd_base); }
    else {
      const struct wmtr_hcmd *c = type < 256 ? cmd_index[kind][type] : NULL;
      if (!c) return 0;
      fields = c->fields; size = c->size;
    }
    if (!x_inside(x, node, size) || !x_in(x, fields, node)) return 0;
    prev = node;
    memcpy(&node, node + offsetof(struct wmtcmd_base, next), 8);
  }
  return 1;
}

static int
x_sm50args(struct xreq *x, uint8_t *node) {
  uint8_t *prev = NULL;
  while (node) {
    if (node <= prev || !x_inside(x, node, sizeof(struct SM50_SHADER_COMPILATION_ARGUMENT_DATA))) return 0;
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *a = (void *)node;
    uint32_t size, ptr_off = 0;
    switch (a->type) {
    case SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT: size = sizeof(struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA);
      ptr_off = offsetof(struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA, elements); break;
    case SM50_SHADER_COMMON: size = sizeof(struct SM50_SHADER_COMMON_DATA); break;
    case SM50_SHADER_PSO_PIXEL_SHADER: size = sizeof(struct SM50_SHADER_PSO_PIXEL_SHADER_DATA); break;
    case SM50_SHADER_IA_INPUT_LAYOUT: size = sizeof(struct SM50_SHADER_IA_INPUT_LAYOUT_DATA);
      ptr_off = offsetof(struct SM50_SHADER_IA_INPUT_LAYOUT_DATA, elements); break;
    case SM50_SHADER_GS_PASS_THROUGH: size = sizeof(struct SM50_SHADER_GS_PASS_THROUGH_DATA); break;
    case SM50_SHADER_PSO_GEOMETRY_SHADER: size = sizeof(struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA); break;
    case SM50_SHADER_PSO_TESSELLATOR: size = sizeof(struct SM50_SHADER_PSO_TESSELLATOR_DATA); break;
    case SM50_SHADER_ROOT_SIGNATURE: case SM50_SHADER_ROOT_SIGNATURE2: size = sizeof(struct SM50_SHADER_ROOT_SIGNATURE_DATA);
      ptr_off = offsetof(struct SM50_SHADER_ROOT_SIGNATURE_DATA, bytecode); break;
    default: return 0;
    }
    if (!x_inside(x, node, size)) return 0;
    uint64_t v;
    if (ptr_off) { memcpy(&v, node + ptr_off, 8); if (v && !x_relocated(x, node + ptr_off)) return 0; }
    memcpy(&v, node, 8);
    if (v && !x_relocated(x, node)) return 0;
    prev = node;
    node = (uint8_t *)(uintptr_t)v;
  }
  return 1;
}

static int
x_ptee_in(struct xreq *x, const struct wmtr_hptee *s, uint8_t *p, const uint8_t *container) {
  uint64_t n = x_count(s, container);
  switch (s->type) {
  case WMTR_HP_STRUCT:
    if (n > (1u << 20) || !x_inside(x, p, n * s->elem_size)) return 0;
    for (uint64_t i = 0; i < n; i++) if (!x_in(x, s->fields, p + i * s->elem_size)) return 0;
    return 1;
  case WMTR_HP_HANDLES_IN:
    if (n > (1u << 24) || !x_inside(x, p, n * 8)) return 0;
    for (uint64_t i = 0; i < n; i++) {
      uint64_t v;
      memcpy(&v, p + i * 8, 8);
      if (!v) continue;
      uint64_t o = x_relocated(x, p + i * 8) ? 0 : resolve_handle(x->h, v, s->mask);
      if (!o) { x->bad = v; return 0; }
      memcpy(p + i * 8, &o, 8);
    }
    return 1;
  case WMTR_HP_HANDLES_OUT0: case WMTR_HP_HANDLE_OUT1:
    if (n > (1u << 24) || !x_inside(x, p, n * 8)) return 0;
    memset(p, 0, n * 8);
    return 1;
  case WMTR_HP_CMDLIST: return x_cmdlist(x, p);
  case WMTR_HP_SM50ARGS: return x_sm50args(x, p);
  default: return 0;
  }
}

static int
x_in(struct xreq *x, const struct wmtr_hfield *f, uint8_t *base) {
  for (; f->flags != WMTR_HF_END; f++) {
    uint8_t *fp = base + f->off;
    uint64_t v;
    memcpy(&v, fp, 8);
    switch (f->flags) {
    case WMTR_HF_IN: {
      if (!v) break;
      uint64_t o = x_relocated(x, fp) ? 0 : resolve_handle(x->h, v, f->mask);
      if (!o) { x->bad = v; return 0; }
      if (x->save && (uint64_t)(fp - x->payload) < x->params_size && x->n_undo < 32)
        x->undo[x->n_undo++] = (typeof(x->undo[0])){(uint32_t)(fp - x->payload), v};
      memcpy(fp, &o, 8);
      break;
    }
    case WMTR_HF_OUT0: case WMTR_HF_OUT1:
      if (x_relocated(x, fp)) { x->bad = v; return 0; }
      memset(fp, 0, 8); /* an implementation that fails leaves it: never the guest's value */
      break;
    case WMTR_HF_PTR:
      if (!v) break;
      if (!x_relocated(x, fp)) { x->bad = v; return 0; }
      if (f->sub && !x_ptee_in(x, f->sub, (uint8_t *)(uintptr_t)v, base)) return 0;
      break;
    }
  }
  return 1;
}

/* a result object: its table handle (0 when the table is full; a +1 result is then released) */
static uint64_t
x_result(struct wmtr_host *h, uint64_t obj, int plus1, int kind, uint64_t hint) {
  if (!obj) return 0;
  if (!kind) {
    kind = classify(obj);
    /* a class that declares none of the protocols classify() knows: the kind the call returns */
    if (kind == WMTR_K_OBJC && hint && !(hint & (hint - 1)) && (hint & WMTR_M_OBJC)) kind = __builtin_ctzll(hint);
  }
  uint64_t r = hinsert(h->htab, obj, kind, plus1, 0);
  if (!r && plus1 && kind <= WMTR_K_LAST_OBJC) [(id)obj release];
  return r;
}

static void
x_out(struct xreq *x, const struct wmtr_hfield *f, uint8_t *base) {
  for (; f->flags != WMTR_HF_END; f++) {
    uint8_t *fp = base + f->off;
    uint64_t v;
    memcpy(&v, fp, 8);
    if (f->flags == WMTR_HF_OUT0 || f->flags == WMTR_HF_OUT1) {
      v = x_result(x->h, v, f->flags == WMTR_HF_OUT1, 0, f->mask);
      memcpy(fp, &v, 8);
    } else if (f->flags == WMTR_HF_PTR && v && f->sub) {
      const struct wmtr_hptee *s = f->sub;
      uint8_t *p = (uint8_t *)(uintptr_t)v;
      uint64_t n = x_count(s, base);
      if (s->type == WMTR_HP_STRUCT && s->fields && x_inside(x, p, n * s->elem_size))
        for (uint64_t i = 0; i < n; i++) x_out(x, s->fields, p + i * s->elem_size);
      else if ((s->type == WMTR_HP_HANDLES_OUT0 || s->type == WMTR_HP_HANDLE_OUT1) && x_inside(x, p, n * 8))
        for (uint64_t i = 0; i < n; i++) {
          uint64_t o;
          memcpy(&o, p + i * 8, 8);
          o = s->type == WMTR_HP_HANDLE_OUT1 ? x_result(x->h, o, 1, __builtin_ctzll(s->mask), 0) : x_result(x->h, o, 0, 0, 0);
          memcpy(p + i * 8, &o, 8);
        }
    }
  }
}

/* ------------------------------------------------------------------------------------------- *
 * overridden calls */
static void
push_free(struct wmtr_host *h, uint32_t blob, uint32_t off, uint64_t size) {
  pthread_mutex_lock(&h->free_lock);
  struct wmtr_ring *R = h->ring;
  uint64_t head = R->free_head;
  while (head - __atomic_load_n(&R->free_tail, __ATOMIC_ACQUIRE) >= WMTR_FREE_RING) {
    pthread_mutex_unlock(&h->free_lock); /* client drains on its next allocation */
    usleep(100);
    pthread_mutex_lock(&h->free_lock);
    head = R->free_head;
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
  case 10: { /* NSAutoreleasePool_alloc_init: a virtual pool, named by the client's handle or a table handle */
    struct unixcall_generic_obj_ret *p = (void *)params;
    int client = r->h.flags & WMTR_F_CLIENT_HANDLE;
    struct vpool *vp = (struct vpool *)(uintptr_t)vpool_push(r->h.slot, client ? p->ret : 0);
    if (!vp) { p->ret = 0; return STATUS_UNSUCCESSFUL; }
    if (!client) {
      pthread_mutex_lock(&vpool_lock);
      vp->hdl = p->ret = hinsert(h->htab, (uint64_t)(uintptr_t)vp, WMTR_K_POOL, 0, 0);
      if (vp->hdl) { vp->htab = h->htab; atomic_fetch_add(&h->htab->refs, 1); } /* a pool may outlive its context */
      pthread_mutex_unlock(&vpool_lock);
      if (!p->ret) { vpool_pop(vp); return STATUS_UNSUCCESSFUL; }
    }
    return STATUS_SUCCESS;
  }
  case 109: case 121: case 122: case 123: case 124:
    return STATUS_NOT_SUPPORTED; /* the client runs listeners itself; mach ports do not cross the VM */
  case 75: case 80: { /* SM50Destroy / SM50FreeError: the handle goes with the object */
    uint64_t o = *(uint64_t *)params;
    NTSTATUS st = ((impl_fn)wmtr_impl_funcs[call])(params);
    hfree_obj(h->htab, o, call == 75 ? WMTR_M_SM50_SHADER : WMTR_M_SM50_ERROR);
    return st;
  }
  case 78: { /* SM50DestroyBitcode: and the token of its data (call 77) */
    uint64_t o = *(uint64_t *)params;
    pthread_mutex_lock(&h->htab->lock);
    uint32_t idx = rev_find(h->htab, o, NULL);
    uint64_t data_tok = idx ? hent_at(h->htab, idx)->aux : 0;
    pthread_mutex_unlock(&h->htab->lock);
    NTSTATUS st = ((impl_fn)wmtr_impl_funcs[call])(params);
    if (data_tok) hfree(h->htab, data_tok);
    hfree_obj(h->htab, o, WMTR_M_SM50_BITCODE);
    return st;
  }
  case 77: { /* SM50GetCompiledBitcode: Data is host memory; the guest gets a token DispatchData_alloc_init accepts */
    struct sm50_get_compiled_bitcode_params *p = (void *)params;
    NTSTATUS st = ((impl_fn)wmtr_impl_funcs[call])(params);
    if (p->data_out && p->data_out->Data) {
      uint64_t tok = hinsert(h->htab, (uint64_t)(uintptr_t)p->data_out->Data, WMTR_K_BITCODE_DATA, 0, p->data_out->Size);
      pthread_mutex_lock(&h->htab->lock);
      uint32_t idx = rev_find(h->htab, (uint64_t)(uintptr_t)p->bitcode, NULL);
      if (idx) hent_at(h->htab, idx)->aux = tok;
      pthread_mutex_unlock(&h->htab->lock);
      p->data_out->Data = (void *)(uintptr_t)tok;
    }
    return st;
  }
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
    uint64_t view = h->ops->create_view(h->ctx, p->hwnd, (void *)p->device, &layer);
    p->ret_view = view ? hinsert(h->htab, view, WMTR_K_VIEW, 0, 0) : 0;
    p->ret_layer = x_result(h, layer, 1, 0, WMTR_M_LAYER);
    if (view && !p->ret_view) { if (h->ops->release_view) h->ops->release_view(h->ctx, view); return STATUS_UNSUCCESSFUL; }
    if (view) view_track(h, view);
    return view ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
  }
  case 73: {
    uint64_t v = ((struct unixcall_generic_obj_noret *)params)->handle;
    if (view_untrack(h, v) && h->ops->release_view) h->ops->release_view(h->ctx, v);
    hfree_obj(h->htab, v, WMTR_M_VIEW);
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
    ((struct unixcall_generic_obj_ret *)params)->ret = hinsert(h->htab, (uint64_t)(uintptr_t)l, WMTR_K_LISTENER, 0, 0);
    return ((struct unixcall_generic_obj_ret *)params)->ret ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
  }
  case 110: { /* destroy: tell the client's listener thread to return; the listener object is kept
                 (late notifications for it are harmless and are dropped by the client) */
    uint64_t lid = ((struct unixcall_generic_obj_noret *)params)->handle;
    if (lid) push_event(h, lid, 0);
    return STATUS_SUCCESS;
  }
  case 17: { /* encodeSignalEvent: with a VR link, the time is kept for the exporter's stage split */
    struct wmtr_vr *vr = atomic_load(&h->vr);
    if (vr) wmtr_vr_note_signal(vr, ((struct unixcall_generic_obj_obj_uint64_noret *)params)->arg0,
                                ((struct unixcall_generic_obj_obj_uint64_noret *)params)->arg1);
    break;
  }
  case 146: /* WMTVRSubmit: the executor's VR exporter (not DXMT's unix side, which has no VR host) */
    return wmtr_vr_submit(atomic_load(&h->vr), &((struct unixcall_wmtvr_submit *)params)->info);
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
  if (__builtin_expect(cs_state == 1, 0)) { /* render-pass profile (rows 220-239) */
    if (call == 32) pp_pass((const struct WMTRenderPassInfo *)((struct unixcall_generic_obj_uint64_obj_ret *)params)->arg);
    else if (call == 12) pp_commit((id<MTLCommandBuffer>)((struct unixcall_generic_obj_noret *)params)->handle);
    else if (call == 31) pp_event("C");
    else if (call == 30 || call == 129) pp_event("B");
    else if (call == 47 || call == 48 || call == 146) pp_event(call == 146 ? "V" : "|");
  }
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

/* the handle a creation carries (WMTR_F_CLIENT_HANDLE); 0: not a client handle (logged; the call fails) */
static uint64_t
client_handle_of(const struct wmtr_req_ext *r, uint8_t *payload) {
  int o = handle_ret_off(r->h.call);
  uint64_t cid = 0;
  if (o >= 0 && o + 8 <= (int)r->h.params_size) memcpy(&cid, payload + o, 8);
  /* pools carry the pool bit, encoders do not; no other call takes a client handle */
  if ((cid & WMTR_REF_MASK) != WMTR_HANDLE_TAG || o < 0 || !(cid & WMTR_HANDLE_POOL) != (r->h.call != 10)) {
    LOG(0, "call %u: bad client handle %llx\n", r->h.call, (unsigned long long)cid);
    return 0;
  }
  return cid;
}

/* after a creation that carried client handle `cid`: the encoder enters the table, the client's entry names it */
static void
handle_created(struct wmtr_host *h, const struct wmtr_req_ext *r, uint8_t *payload, uint64_t cid) {
  if (cid & WMTR_HANDLE_POOL) return;
  uint64_t obj;
  memcpy(&obj, payload + handle_ret_off(r->h.call), 8);
  uint64_t t = obj ? x_result(h, obj, 0, 0, r->h.call == 30 ? WMTR_M_ENC_BLIT : r->h.call == 31 ? WMTR_M_ENC_COMPUTE : WMTR_M_ENC_RENDER) : 0;
  struct wmtr_handle *e = &h->handles[cid % WMTR_HANDLE_SLOTS];
  atomic_store_explicit(&e->obj, t, memory_order_relaxed);
  atomic_store_explicit(&e->id, cid, memory_order_release);
  if (!obj) LOG(0, "call %s: creation of client handle %llx failed; the calls using it fail\n", call_name[r->h.call],
                (unsigned long long)cid);
  memcpy(payload + handle_ret_off(r->h.call), &cid, 8);
}

/* NSObject_retain / NSObject_release: references the guest owns are counted per handle, so a release without a
 * matching retain or +1 result is refused instead of freeing an object others use */
static NTSTATUS
host_retain_release(struct wmtr_host *h, uint16_t call, uint64_t v) {
  if (!v) return STATUS_SUCCESS;
  if ((v & WMTR_REF_MASK) == WMTR_HANDLE_TAG) {
    if (v & WMTR_HANDLE_POOL) {
      uint64_t p = vpool_find(v);
      if (!p) return STATUS_INVALID_HANDLE;
      if (call == 1) vpool_pop((struct vpool *)(uintptr_t)p);
      return STATUS_SUCCESS;
    }
    struct wmtr_handle *c = &h->handles[v % WMTR_HANDLE_SLOTS];
    if (atomic_load_explicit(&c->id, memory_order_acquire) != v) return STATUS_INVALID_HANDLE;
    v = atomic_load_explicit(&c->obj, memory_order_relaxed);
  }
  struct wmtr_htab *t = h->htab;
  pthread_mutex_lock(&t->lock);
  struct hent *e = hentry(t, v, WMTR_M_OBJC | WMTR_M_POOL);
  if (!e) { pthread_mutex_unlock(&t->lock); return STATUS_INVALID_HANDLE; }
  uint64_t obj = atomic_load_explicit(&e->obj, memory_order_relaxed);
  if ((atomic_load_explicit(&e->key, memory_order_relaxed) & 0xff) == WMTR_K_POOL) {
    pthread_mutex_unlock(&t->lock);
    if (call == 1) vpool_pop((struct vpool *)(uintptr_t)obj);
    return STATUS_SUCCESS;
  }
  if (call == 0) e->grefs++;
  else if (!e->grefs) {
    pthread_mutex_unlock(&t->lock);
    LOG(1, "release of %llx: the client holds no reference; refused\n", (unsigned long long)v);
    return STATUS_INVALID_HANDLE;
  } else e->grefs--;
  pthread_mutex_unlock(&t->lock);
  if (call == 0) [(id)obj retain];
  else [(id)obj release];
  return STATUS_SUCCESS;
}

/* DispatchData_alloc_init's source: relocated guest bytes, or the token of compiled bitcode (call 77) no longer
 * than that bitcode */
static int
x_dispatch_data(struct xreq *x) {
  struct unixcall_generic_obj_uint64_obj_ret *p = (void *)x->payload;
  if (!p->handle || x_relocated(x, x->payload)) return 1;
  struct hent *e = hentry(x->h->htab, p->handle, WMTR_M_BITCODE_DATA);
  if (!e || p->arg > e->aux) { x->bad = p->handle; return 0; }
  x->undo[x->n_undo++] = (typeof(x->undo[0])){0, p->handle};
  p->handle = atomic_load_explicit(&e->obj, memory_order_relaxed);
  return 1;
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
  uint64_t cid = 0;
  uint16_t call = r->h.call;
  int is_call = !(r->h.flags & WMTR_F_CONTROL) && call < WMTR_NCALLS;
  /* which payload words the relocations rewrote: a pointer field must be one of them (or NULL) */
  static __thread uint64_t *relmap;
  static __thread uint32_t relmap_words;
  int use_map = is_call && (call_needs[call] & NEED_PTR);
  if (use_map) {
    uint32_t words = (r->payload_size >> 9) + 1;
    if (words > relmap_words) { free(relmap); relmap = malloc(8u * words); relmap_words = relmap ? words : 0; }
    if (relmap) memset(relmap, 0, 8u * words);
    else use_map = 0;
  }
  int nd = 0; /* relocations done (orig[] valid) */
  for (int i = 0; i < nr; i++) {
    if (r->payload_size < 8 || relocs[i] > r->payload_size - 8) { st = STATUS_INVALID_PARAMETER; break; }
    uint64_t v;
    memcpy(&v, payload + relocs[i], 8);
    orig[i] = v;
    void *p = v ? resolve(h, v, payload, r->payload_size) : NULL;
    if (v && !p) { st = STATUS_INVALID_PARAMETER; LOG(0, "call %u: bad reference %llx\n", r->h.call, (unsigned long long)v); break; }
    memcpy(payload + relocs[i], &p, 8);
    if (use_map && !(relocs[i] & 7)) relmap[relocs[i] >> 9] |= 1ull << ((relocs[i] >> 3) & 63);
    nd = i + 1;
  }
  struct xreq x;
  x.n_undo = 0;
  if (st == STATUS_SUCCESS) {
    if (r->h.flags & WMTR_F_CONTROL) {
      if (r->h.call == WMTR_CTL_VR_QUERY) /* answered from cached state: never blocks the executor */
        st = wmtr_vr_query(atomic_load(&h->vr), payload, r->h.params_size);
      else
        st = STATUS_SUCCESS; /* PING and HELLO need no work */
    } else if (r->h.call < WMTR_NCALLS) {
      if (r->h.params_size < call_params_size[r->h.call]) {
        st = STATUS_INVALID_PARAMETER; /* the implementation would read and write past the params */
        LOG(0, "call %u: params %u bytes, expected %u\n", r->h.call, r->h.params_size, call_params_size[r->h.call]);
      } else if (__atomic_load_n(&h->faulted, __ATOMIC_ACQUIRE)) {
        st = STATUS_ACCESS_VIOLATION; /* this client already faulted the executor: refuse further work */
      } else if ((r->h.flags & WMTR_F_CLIENT_HANDLE) && !(cid = client_handle_of(r, payload))) {
        st = STATUS_INVALID_HANDLE;
      } else if (call == 0 || call == 1) {
        st = host_retain_release(h, call, *(uint64_t *)payload);
      } else if ((x.h = h, x.payload = payload, x.psize = r->payload_size, x.params_size = r->h.params_size,
                  x.relmap = use_map ? relmap : NULL, x.call = call, x.save = r->h.flags & WMTR_F_SYNC, x.bad = 0,
                  !(call == 114 ? x_dispatch_data(&x) : 1) || (wmtr_call_hfields[call] && !x_in(&x, wmtr_call_hfields[call], payload)))) {
        /* a handle that names nothing of the expected kind, or a pointer the request did not relocate: refused */
        st = STATUS_INVALID_HANDLE;
        atomic_fetch_add_explicit(&h->n_refused, 1, memory_order_relaxed);
        LOG(1, "call %u (%s): refused handle or pointer %llx\n", call, call_name[call], (unsigned long long)x.bad);
      } else {
        sigjmp_buf jb;
        if (sigsetjmp(jb, 1) == 0) {
          fault_jmp = &jb;
          int cs = callstats_on();
          uint64_t t0 = cs ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
          @autoreleasepool {
            @try {
              st = host_call(h, r->h.call, payload, r, relocs, orig);
            } @catch (id e) { /* e.g. a selector the object does not implement: contained like a fault */
              LOG(0, "call %u raised %s; failing this client's calls from now on\n", call, [[e description] UTF8String]);
              __atomic_store_n(&h->faulted, 1, __ATOMIC_RELEASE);
              st = STATUS_ACCESS_VIOLATION;
            }
          }
          if (cs) callstats_add(r->h.call, r->h.flags & WMTR_F_SYNC, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
          fault_jmp = NULL;
          if (cid) handle_created(h, r, payload, cid); /* the client named the result itself */
          else if (call_needs[call] & NEED_OUT) x_out(&x, wmtr_call_hfields[call], payload); /* results become handles */
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
  if (r->h.flags & WMTR_F_SYNC) {
    /* the reply carries the caller's own values: its handles, and its wire references instead of host addresses */
    for (uint32_t i = 0; i < x.n_undo; i++) memcpy(payload + x.undo[i].off, &x.undo[i].v, 8);
    for (int i = 0; i < nd; i++)
      if (relocs[i] + 8 <= r->h.params_size) memcpy(payload + relocs[i], &orig[i], 8);
  }
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
    uint8_t cls = hdr.call < WMTR_NCALLS && !(hdr.flags & WMTR_F_CONTROL) ? call_class[hdr.call] : WMTR_CLASS_S;
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

/* WMTR_VR_FD: one link per process while contexts are alive. Proton's steam.exe may ask the runtime for the
 * headset before the game creates its device: the contexts share the link instead of the first one claiming
 * it. The last context closes the duplicated fd so the launcher can stop the host; a later context reattaches. */
static struct wmtr_vr *shared_vr;
static unsigned shared_vr_users;
static pthread_mutex_t shared_vr_lock = PTHREAD_MUTEX_INITIALIZER;

struct wmtr_vr *
wmtr_host_vr(struct wmtr_host *h) {
  return atomic_load(&h->vr);
}

static struct wmtr_vr *
vr_shared_link(const char *vf, const char *vm) {
  pthread_mutex_lock(&shared_vr_lock);
  if (!shared_vr) {
    int fd = dup(atoi(vf)); /* a dup: closing it would not end the host's session */
    shared_vr = fd >= 0 ? wmtr_vr_create(fd, vm) : NULL;
    if (!shared_vr && fd >= 0) close(fd);
    LOG(shared_vr ? 1 : 0, "VR link %s (fd %s, %s)\n", shared_vr ? "attached" : "failed", vf, vm);
  }
  struct wmtr_vr *vr = shared_vr;
  if (vr) shared_vr_users++;
  pthread_mutex_unlock(&shared_vr_lock);
  return vr;
}

static void
vr_shared_release(struct wmtr_vr *vr) {
  pthread_mutex_lock(&shared_vr_lock);
  if (vr && vr == shared_vr && shared_vr_users && !--shared_vr_users) {
    shared_vr = NULL;
    callstats_vr(vr, NULL);
    wmtr_vr_destroy(vr);
  }
  pthread_mutex_unlock(&shared_vr_lock);
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
  h->htab = htab_create();
  static pthread_once_t xlate_once = PTHREAD_ONCE_INIT;
  pthread_once(&xlate_once, xlate_init);
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
  const char *vs = getenv("WMTR_VR_SOCKET"), *vf = getenv("WMTR_VR_FD"), *vm = getenv("WMTR_VR_MACH_NAME");
  if (vf && *vf && vm && *vm) {
    /* steamac-vm (same process) holds the socketpair end and spawned steamac-vrhost with the other one */
    struct wmtr_vr *vr = vr_shared_link(vf, vm);
    if (vr) { atomic_store(&h->vr, vr); h->owns_vr = 2; callstats_vr(NULL, vr); } /* shared link ref held until destroy */
  } else if (vs && *vs && vm && *vm) {
    struct wmtr_vr *vr = wmtr_vr_connect(vs, vm);
    LOG(vr ? 1 : 0, "VR link %s (%s, %s)\n", vr ? "connected" : "failed", vs, vm);
    if (vr) { atomic_store(&h->vr, vr); h->owns_vr = 1; callstats_vr(NULL, vr); }
  }
  pthread_create(&h->thread, &a, executor, h);
  return h;
}

void
wmtr_host_set_vr(struct wmtr_host *h, struct wmtr_vr *vr) {
  callstats_vr(atomic_load(&h->vr), vr);
  atomic_store(&h->vr, vr);
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
  if (h->owns_vr) {
    struct wmtr_vr *vr = atomic_exchange(&h->vr, NULL);
    if (h->owns_vr == 2) vr_shared_release(vr);
    else { callstats_vr(vr, NULL); wmtr_vr_destroy(vr); }
    h->owns_vr = 0;
  }
  htab_unref(h->htab); /* entries of objects still alive keep it until their watchers go */
  h->htab = NULL;
}

/* test hooks: put a host object into a context's table as if a call had returned it; the object a handle names
 * (any kind, 0 if none); requests refused for a bad handle or pointer */
__attribute__((visibility("default"))) uint64_t
wmtr_host_debug_handle(struct wmtr_host *h, uint64_t obj, int kind, int plus1) {
  /* kind 0: a real object, classified and watched like a call's result; else a test's stand-in, never touched */
  return hinsert_w(h->htab, obj, kind ? kind : classify(obj), plus1, 0, !kind);
}
__attribute__((visibility("default"))) uint64_t
wmtr_host_debug_object(struct wmtr_host *h, uint64_t handle) {
  return resolve_handle(h, handle, ~0ull);
}
__attribute__((visibility("default"))) int
wmtr_host_debug_kind(struct wmtr_host *h, uint64_t handle) {
  struct hent *e = hentry(h->htab, handle, ~0ull);
  return e ? (int)(atomic_load(&e->key) & 0xff) : -1;
}
__attribute__((visibility("default"))) uint64_t
wmtr_host_refused(struct wmtr_host *h) {
  return atomic_load(&h->n_refused);
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
