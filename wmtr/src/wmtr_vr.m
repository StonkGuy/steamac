/* wmtr VR export: see include/wmtr_vr.h. Manual retain/release, like wmtr_host.m. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOSurface/IOSurface.h>
#include <servers/bootstrap.h>
#include <mach/mach.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdatomic.h>
#include <pthread.h>
#include <os/lock.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WINEMETAL_API
#include "winemetal_thunks.h"
#include "vrlink.h"
#include "steamac_vr_guest.h"
#include "wmtr_vr.h"

_Static_assert(VRL_INPUT_COUNT == SVR_IN_COUNT, "vrlink.h and the guest protocol disagree on input ids");
_Static_assert((int)VRL_DEV_COUNT == (int)SVR_DEV_COUNT && sizeof(struct vrl_pose) == sizeof(struct svr_pose), "pose layout");
/* the guest runtime builds WMTVRSubmitInfo from its own mirror of the struct */
_Static_assert(sizeof(struct svr_wmtvr_submit) == sizeof(struct WMTVRSubmitInfo) &&
                   offsetof(struct svr_wmtvr_submit, wait_value) == offsetof(struct WMTVRSubmitInfo, wait_value) &&
                   offsetof(struct svr_wmtvr_submit, fov) == offsetof(struct WMTVRSubmitInfo, fov),
               "svr_wmtvr_submit must match DXMT's WMTVRSubmitInfo");

#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER ((int)0xC000000D)
#define STATUS_NOT_SUPPORTED ((int)0xC00000BB)
#define STATUS_UNSUCCESSFUL ((int)0xC0000001)

#define VR_IMAGES 3 /* per eye: one being written, one in the compositor, one spare */
#define VR_SIGNALS 64 /* DXMT signals its queue event once per command chunk: several frames' worth */

struct vr_swapchain {
  uint32_t id, width, height, format, flags;
  IOSurfaceRef surface[VR_IMAGES];
  id<MTLTexture> image[VR_IMAGES];
  _Atomic int busy[VR_IMAGES]; /* between the blit and VRL_RELEASE */
};

struct wmtr_vr {
  int fd;
  char mach_name[128];
  mach_port_t service;
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  struct vr_swapchain sc[2]; /* one per eye; ids 1 and 2 */
  uint32_t generation;       /* bumps swapchain ids on recreate: id = eye + 1 + 2 * generation */
  _Atomic uint64_t submits, sent, dropped, released, errors, swapchains;
  int warned;
  _Atomic int inflight; /* command buffers whose completion handler has not run */
  /* newest host state for the guest's polls; under `lock` (the reader thread and the executor both drain) */
  pthread_mutex_t lock;
  pthread_t reader;
  _Atomic int quit;
  struct vrl_state state;
  struct vrl_timing timing;
  struct wmtr_vr_cadence cadence; /* SVR_FRAME ids/times (wmtr_vr_frame_cadence), under `lock` */
  struct vrl_poses poses;
  struct vrl_input input;
  _Atomic uint64_t queries, haptics;
  _Atomic int have_state; /* a VRL_STATE arrived on this link */
  int64_t state_wait_ns;  /* when a hello last waited for it */
  /* where a frame's time between WMTVRSubmit and VRL_SUBMIT goes (wmtr_vr_stats.stage_ns) */
  _Atomic uint64_t stage_frames, stage_ns[WMTR_VR_STAGES];
  /* the newest shared-event signals the executor encoded for the guest (wmtr_vr_note_signal), under sig_lock */
  os_unfair_lock sig_lock;
  struct vr_signal { uint64_t event, value; int64_t t; } sig[VR_SIGNALS];
  unsigned sig_n;
};

/* a runtime hello on a link without host state yet: how long it waits, how often it asks the host again, and how
 * long until the next hello waits again (a host that never answers must not slow every hello down) */
#define VR_STATE_WAIT_NS 250000000ll
#define VR_STATE_ASK_NS 50000000ll
#define VR_STATE_WAIT_AGAIN_NS 2000000000ll

_Static_assert(WMTR_VR_FRAME_FROM_PACER == SVR_FRAME_FROM_PACER && WMTR_VR_FRAME_PACER_LIVE == SVR_FRAME_PACER_LIVE,
               "cadence flags are the guest's SVR_FRAME_* flags");

static int64_t
now_ns(void) {
  return (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void
vr_log(struct wmtr_vr *vr, const char *msg) {
  if (vr->warned++ < 16) fprintf(stderr, "wmtr-vr: %s\n", msg);
}

/* IOSurface layout for the pixel formats VR eye textures use */
static int
format_info(uint32_t mtl, uint32_t *fourcc, uint32_t *bpe) {
  switch (mtl) {
  case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: *fourcc = 'RGBA'; *bpe = 4; return 1;
  case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB: *fourcc = 'BGRA'; *bpe = 4; return 1;
  case MTLPixelFormatRGBA16Float: *fourcc = 'RGhA'; *bpe = 8; return 1;
  default: return 0;
  }
}

static int
send_packet(struct wmtr_vr *vr, const void *p, size_t n) {
  ssize_t r;
  do r = send(vr->fd, p, n, 0); while (r < 0 && errno == EINTR);
  if (r != (ssize_t)n) { atomic_fetch_add(&vr->errors, 1); return 0; }
  return 1;
}

static mach_port_t
service_port(struct wmtr_vr *vr) {
  if (vr->service == MACH_PORT_NULL &&
      bootstrap_look_up(bootstrap_port, vr->mach_name, &vr->service) != KERN_SUCCESS)
    vr->service = MACH_PORT_NULL;
  return vr->service;
}

static void
swapchain_free(struct vr_swapchain *s) {
  for (int i = 0; i < VR_IMAGES; i++) {
    [s->image[i] release]; s->image[i] = nil;
    if (s->surface[i]) CFRelease(s->surface[i]);
    s->surface[i] = NULL;
    atomic_store(&s->busy[i], 0);
  }
  s->id = 0;
}

/* (re)create eye `eye`'s swapchain: images, ports to the host (Mach first), then VRL_SWAPCHAIN */
static int
swapchain_make(struct wmtr_vr *vr, int eye, uint32_t w, uint32_t h, uint32_t format, uint32_t flags) {
  struct vr_swapchain *s = &vr->sc[eye];
  uint32_t fourcc, bpe;
  if (!format_info(format, &fourcc, &bpe)) { vr_log(vr, "unsupported eye texture format"); return 0; }
  mach_port_t svc = service_port(vr);
  if (svc == MACH_PORT_NULL) { vr_log(vr, "vrhost mach service not found"); return 0; }
  swapchain_free(s);
  vr->generation++;
  s->id = (uint32_t)eye + 1 + 2 * vr->generation;
  s->width = w; s->height = h; s->format = format; s->flags = flags;
  struct vrl_mach_swapchain m;
  memset(&m, 0, sizeof m);
  for (int i = 0; i < VR_IMAGES; i++) {
    NSDictionary *props = @{
      (id)kIOSurfaceWidth : @(w), (id)kIOSurfaceHeight : @(h), (id)kIOSurfaceBytesPerElement : @(bpe),
      (id)kIOSurfacePixelFormat : @(fourcc),
    };
    s->surface[i] = IOSurfaceCreate((CFDictionaryRef)props);
    if (!s->surface[i]) { swapchain_free(s); vr_log(vr, "IOSurfaceCreate failed"); return 0; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    s->image[i] = [vr->device newTextureWithDescriptor:d iosurface:s->surface[i] plane:0];
    if (!s->image[i]) { swapchain_free(s); vr_log(vr, "IOSurface texture failed"); return 0; }
    m.ports[i].name = IOSurfaceCreateMachPort(s->surface[i]);
    m.ports[i].disposition = MACH_MSG_TYPE_MOVE_SEND;
    m.ports[i].type = MACH_MSG_PORT_DESCRIPTOR;
  }
  m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
  m.header.msgh_size = sizeof m;
  m.header.msgh_remote_port = svc;
  m.header.msgh_id = VRL_MACH_SWAPCHAIN_ID;
  m.body.msgh_descriptor_count = VRL_MAX_IMAGES;
  for (int i = VR_IMAGES; i < VRL_MAX_IMAGES; i++) { m.ports[i].name = MACH_PORT_NULL; m.ports[i].disposition = MACH_MSG_TYPE_COPY_SEND; m.ports[i].type = MACH_MSG_PORT_DESCRIPTOR; }
  m.swapchain_id = s->id;
  m.image_count = VR_IMAGES;
  kern_return_t kr = mach_msg(&m.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m, 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL);
  if (kr != KERN_SUCCESS) {
    for (int i = 0; i < VR_IMAGES; i++) mach_port_deallocate(mach_task_self(), m.ports[i].name);
    mach_port_deallocate(mach_task_self(), vr->service);
    vr->service = MACH_PORT_NULL;
    swapchain_free(s);
    vr_log(vr, "mach send of swapchain ports failed");
    return 0;
  }
  struct vrl_swapchain p = {{VRL_SWAPCHAIN, sizeof p}, s->id, VR_IMAGES, w, h, format, flags};
  if (!send_packet(vr, &p, sizeof p)) { swapchain_free(s); return 0; }
  atomic_fetch_add(&vr->swapchains, 1);
  return 1;
}

/* host packets (non-blocking): VRL_RELEASE marks images free; state, timing, poses and input replace the cached
 * copies the guest's polls are answered from. Called by the reader thread and on the executor before a submit or
 * query (recursive lock: wmtr_vr_query holds it while answering). */
static void
drain_releases(struct wmtr_vr *vr) {
  pthread_mutex_lock(&vr->lock);
  union {
    struct vrl_header h; struct vrl_release r; struct vrl_state s; struct vrl_timing t; struct vrl_poses p;
    struct vrl_input i; uint8_t raw[2048];
  } u;
  for (;;) {
    ssize_t n = recv(vr->fd, &u, sizeof u, MSG_DONTWAIT);
    if (n <= 0) break;
    if (n < (ssize_t)sizeof u.h || u.h.size != (uint32_t)n) continue;
    switch (u.h.type) {
    case VRL_RELEASE:
      if (n < (ssize_t)sizeof u.r) break;
      for (int e = 0; e < 2; e++)
        if (vr->sc[e].id == u.r.swapchain && u.r.image < VR_IMAGES) atomic_store(&vr->sc[e].busy[u.r.image], 0);
      atomic_fetch_add(&vr->released, 1);
      break;
    case VRL_STATE:
      if (n == sizeof u.s) {
        vr->state = u.s;
        atomic_store(&vr->have_state, 1);
      }
      break;
    case VRL_TIMING: if (n == sizeof u.t) vr->timing = u.t; break;
    case VRL_POSES: if (n == sizeof u.p) vr->poses = u.p; break;
    case VRL_INPUT: if (n == sizeof u.i) vr->input = u.i; break;
    default: break;
    }
  }
  pthread_mutex_unlock(&vr->lock);
}

/* The host sends timing and poses every frame whether or not the guest calls in; read them as they come so they
 * never fill the socket and hold up VRL_RELEASE (a full receive buffer blocks the host's sends). One wake-up per
 * host packet (about three per frame), only while VR is on. */
static void *
reader_main(void *arg) {
  struct wmtr_vr *vr = arg;
  pthread_setname_np("wmtr-vr-link");
  while (!atomic_load(&vr->quit)) {
    struct pollfd pf = {vr->fd, POLLIN, 0};
    int r = poll(&pf, 1, 100);
    if (r > 0 && (pf.revents & POLLIN)) drain_releases(vr);
    else if (r > 0) usleep(10000); /* peer gone: nothing to read, keep the loop cheap until destroy */
  }
  return NULL;
}

unsigned
wmtr_vr_frame_cadence(struct wmtr_vr_cadence *c, uint64_t host_id, int64_t host_wake, int64_t host_display,
                      uint64_t guest_last, int64_t now, int64_t period, int64_t render_period, uint64_t *id,
                      int64_t *wake, int64_t *display) {
  int64_t window = 4 * period > WMTR_VR_PACER_LIVE_NS ? 4 * period : WMTR_VR_PACER_LIVE_NS;
  int live = host_id && host_wake > now - window;                               /* the pacer is running */
  int fresh = host_id && host_id != c->host_last && host_wake > now - 4 * period; /* a new, current pacer frame */
  if (fresh) {
    if (host_id < c->host_last || (int64_t)host_id + c->offset <= (int64_t)guest_last)
      c->offset = (int64_t)guest_last + 1 - (int64_t)host_id; /* host restarted: continue after the guest's id */
    c->host_last = host_id;
    *id = (uint64_t)((int64_t)host_id + c->offset);
    *wake = host_wake, *display = host_display;
  } else if (live) {
    /* the pacer's next frame is not out yet: a local one at the time it is due, for a guest that does not wait;
     * a period after the last one at least, or a guest that asks again at once (one that does not wait, or a
     * runtime that gave up waiting) renders a burst of frames, all due now, faster than it can display them */
    *id = guest_last + 1;
    int64_t w = host_wake + (render_period > period ? render_period : period);
    if (w < now) w = now;
    if (c->last_wake && w < c->last_wake + period) w = c->last_wake + period;
    *wake = w, *display = host_display + (w - host_wake);
  } else {
    /* no new host frame: the next local one, a period after the last, never in the past by more than a frame */
    *id = guest_last + 1;
    int64_t w = c->last_wake ? c->last_wake + period : now;
    if (w < now - period) w = now;
    *wake = w, *display = w + 2 * period;
  }
  if (*id <= guest_last) *id = guest_last + 1;
  if (*wake > now + 2 * period) { /* never sleep on a timing far ahead */
    *display -= *wake - now;
    *wake = now;
  }
  c->last_wake = *wake;
  return (fresh ? WMTR_VR_FRAME_FROM_PACER : 0) | (live ? WMTR_VR_FRAME_PACER_LIVE : 0);
}

static void
fill_view(const struct vrl_state *st, float fov[2][4], float *ipd) {
  memcpy(fov, st->fov, sizeof st->fov);
  *ipd = st->ipd_m;
}

static int
query_locked(struct wmtr_vr *vr, void *buf, uint32_t size) {
  struct svr_hdr *q = buf;
  if (size < sizeof *q || q->size > size) return STATUS_INVALID_PARAMETER;
  if (vr) {
    drain_releases(vr);
    atomic_fetch_add(&vr->queries, 1);
  }
  const struct vrl_state *st = vr ? &vr->state : NULL;
  int connected = st && st->connected;
  switch (q->op) {
  case SVR_HELLO: {
    if (q->size != sizeof(struct svr_hello_req) || size < sizeof(struct svr_hello_rep)) return STATUS_INVALID_PARAMETER;
    struct svr_hello_rep r = {{SVR_HELLO, sizeof r}};
    r.version = SVR_VERSION;
    r.connected = connected;
    if (st) {
      r.eye_width = st->eye_width, r.eye_height = st->eye_height, r.refresh_hz = st->refresh_hz, r.profile = st->profile;
      fill_view(st, r.fov, &r.ipd_m);
      memcpy(r.name, st->name, sizeof r.name);
      r.name[sizeof r.name - 1] = 0;
    }
    memcpy(buf, &r, sizeof r);
    return STATUS_SUCCESS;
  }
  case SVR_FRAME: {
    if (q->size != sizeof(struct svr_frame_req) || size < sizeof(struct svr_frame_rep)) return STATUS_INVALID_PARAMETER;
    uint64_t last = ((struct svr_frame_req *)buf)->last_frame_id;
    struct svr_frame_rep r = {{SVR_FRAME, sizeof r}};
    int64_t now = now_ns();
    float hz = vr && vr->timing.refresh_hz > 0 ? vr->timing.refresh_hz : st && st->refresh_hz > 0 ? st->refresh_hz : 90.0f;
    int64_t period = (int64_t)(1e9f / hz);
    int64_t render_period = vr && vr->timing.render_divisor > 1 ? period * vr->timing.render_divisor : period;
    /* the pacer's next frame, or the local cadence with no headset (WaitGetPoses keeps pacing either way) */
    uint64_t f;
    int64_t wake, disp;
    struct wmtr_vr_cadence none = {0};
    r.flags = wmtr_vr_frame_cadence(vr ? &vr->cadence : &none, vr ? vr->timing.frame_id : 0,
                                    vr ? vr->timing.wake_up : 0, vr ? vr->timing.predicted_display : 0, last, now,
                                    period, render_period, &f, &wake, &disp);
    r.frame_id = f, r.host_now = now, r.wake_up = wake, r.predicted_display = disp;
    r.refresh_hz = hz;
    r.connected = connected;
    if (st) r.profile = st->profile, fill_view(st, r.fov, &r.ipd_m);
    if (vr && vr->poses.time) {
      memcpy(r.pose, vr->poses.pose, sizeof r.pose);
      r.pose_time = vr->poses.time;
    } else {
      r.pose_time = disp;
    }
    if (vr && vr->input.time) {
      memcpy(r.input, vr->input.value, sizeof r.input);
      if (vr->input.profile) r.profile = vr->input.profile;
    }
    memcpy(buf, &r, sizeof r);
    return STATUS_SUCCESS;
  }
  case SVR_HAPTIC: {
    if (q->size != sizeof(struct svr_haptic_req) || size < sizeof(struct svr_ack)) return STATUS_INVALID_PARAMETER;
    const struct svr_haptic_req *h = buf;
    struct vrl_haptics p = {{VRL_HAPTICS, sizeof p}, h->hand, h->duration_s, h->frequency, h->amplitude};
    int ok = vr && connected && send_packet(vr, &p, sizeof p);
    if (ok) atomic_fetch_add(&vr->haptics, 1);
    struct svr_ack a = {{SVR_HAPTIC, sizeof a}, ok ? 0 : -107 /* ENOTCONN */, 0};
    memcpy(buf, &a, sizeof a);
    return STATUS_SUCCESS;
  }
  default:
    return STATUS_INVALID_PARAMETER;
  }
}

int
wmtr_vr_query(struct wmtr_vr *vr, void *buf, uint32_t size) {
  if (!vr) return query_locked(NULL, buf, size);
  /* The host answers VRL_HELLO with its state (headset connected, eye size). A runtime that starts with the link
   * (VR_Init right after the process attached: Proton's steam.exe, a relaunched game) would read "no headset,
   * 0x0" before that state arrives, or for good when the host's answer was lost (it is sent without waiting, and a
   * link socket left full by the previous link drops it): its hello waits for the state and asks the host again. */
  if (size >= sizeof(struct svr_hdr) && ((struct svr_hdr *)buf)->op == SVR_HELLO && !atomic_load(&vr->have_state)) {
    pthread_mutex_lock(&vr->lock);
    int64_t start = now_ns(), asked = start;
    int wait = !vr->state_wait_ns || start - vr->state_wait_ns > VR_STATE_WAIT_AGAIN_NS;
    if (wait) vr->state_wait_ns = start;
    pthread_mutex_unlock(&vr->lock);
    while (wait && !atomic_load(&vr->have_state) && now_ns() - start < VR_STATE_WAIT_NS) {
      drain_releases(vr);
      if (atomic_load(&vr->have_state)) break;
      usleep(1000);
      if (now_ns() - asked > VR_STATE_ASK_NS) {
        struct vrl_hello hello = {{VRL_HELLO, sizeof hello}, VRL_VERSION, (uint32_t)getpid()};
        send_packet(vr, &hello, sizeof hello);
        asked = now_ns();
      }
    }
  }
  pthread_mutex_lock(&vr->lock);
  int r = query_locked(vr, buf, size);
  pthread_mutex_unlock(&vr->lock);
  return r;
}

void
wmtr_vr_note_signal(struct wmtr_vr *vr, uint64_t event, uint64_t value) {
  if (!vr) return;
  int64_t t = now_ns();
  os_unfair_lock_lock(&vr->sig_lock);
  vr->sig[vr->sig_n++ % VR_SIGNALS] = (struct vr_signal){event, value, t};
  os_unfair_lock_unlock(&vr->sig_lock);
}

/* when the executor encoded the signal that takes `event` to `value` (the first one at or past it); 0 = not seen */
static int64_t
signal_time(struct wmtr_vr *vr, uint64_t event, uint64_t value) {
  int64_t t = 0;
  uint64_t best = UINT64_MAX;
  os_unfair_lock_lock(&vr->sig_lock);
  for (int i = 0; i < VR_SIGNALS; i++)
    if (vr->sig[i].event == event && vr->sig[i].value >= value && vr->sig[i].value < best) best = vr->sig[i].value, t = vr->sig[i].t;
  os_unfair_lock_unlock(&vr->sig_lock);
  return t;
}

/* One frame's stages from its submit to the completion handler of its copy. The copy's command buffer waits for
 * the guest's event, so its GPU start is the end of the guest's rendering. */
static void
stage_account(struct wmtr_vr *vr, int64_t t_submit, int64_t t_signal, id<MTLCommandBuffer> cb, int64_t t_done) {
  int64_t gs = (int64_t)(cb.GPUStartTime * 1e9), ge = (int64_t)(cb.GPUEndTime * 1e9);
  if (gs < t_submit || gs > t_done) gs = t_done; /* no GPU times: the whole wait counts as the guest's */
  if (ge < gs || ge > t_done) ge = gs;
  if (t_signal < t_submit || t_signal > gs) t_signal = t_submit; /* encoded before the submit, or not seen */
  atomic_fetch_add_explicit(&vr->stage_ns[WMTR_VR_STAGE_ENCODE], (uint64_t)(t_signal - t_submit), memory_order_relaxed);
  atomic_fetch_add_explicit(&vr->stage_ns[WMTR_VR_STAGE_RENDER], (uint64_t)(gs - t_signal), memory_order_relaxed);
  atomic_fetch_add_explicit(&vr->stage_ns[WMTR_VR_STAGE_COPY], (uint64_t)(ge - gs), memory_order_relaxed);
  atomic_fetch_add_explicit(&vr->stage_ns[WMTR_VR_STAGE_NOTIFY], (uint64_t)(t_done - ge), memory_order_relaxed);
  atomic_fetch_add_explicit(&vr->stage_frames, 1, memory_order_relaxed);
}

int
wmtr_vr_submit(struct wmtr_vr *vr, const struct WMTVRSubmitInfo *info) {
  if (!vr) return STATUS_NOT_SUPPORTED;
  int64_t t_submit = now_ns();
  atomic_fetch_add(&vr->submits, 1);
  drain_releases(vr);
  id<MTLTexture> src[2];
  uint32_t rx[2], ry[2], rw[2], rh[2];
  for (int e = 0; e < 2; e++) {
    const struct WMTVREye *in = &info->eye[e];
    src[e] = (id<MTLTexture>)in->texture;
    if (!src[e] || src[e].sampleCount != 1 || in->slice >= src[e].arrayLength) {
      atomic_fetch_add(&vr->errors, 1); vr_log(vr, "bad eye texture"); return STATUS_INVALID_PARAMETER;
    }
    rx[e] = in->x; ry[e] = in->y;
    rw[e] = in->w ? in->w : (uint32_t)src[e].width;
    rh[e] = in->w ? in->h : (uint32_t)src[e].height;
    if (!rw[e] || !rh[e] || (uint64_t)rx[e] + rw[e] > src[e].width || (uint64_t)ry[e] + rh[e] > src[e].height) {
      atomic_fetch_add(&vr->errors, 1); vr_log(vr, "eye rect out of bounds"); return STATUS_INVALID_PARAMETER;
    }
    if (!vr->device) { vr->device = [src[e].device retain]; vr->queue = [vr->device newCommandQueue]; }
    uint32_t fmt = (uint32_t)src[e].pixelFormat;
    uint32_t flags = (in->flags & WMTVR_EYE_SRGB) || fmt == MTLPixelFormatRGBA8Unorm_sRGB || fmt == MTLPixelFormatBGRA8Unorm_sRGB
                         ? VRL_SWAPCHAIN_SRGB : 0;
    struct vr_swapchain *s = &vr->sc[e];
    if (!s->id || s->width != rw[e] || s->height != rh[e] || s->format != fmt || s->flags != flags)
      if (!swapchain_make(vr, e, rw[e], rh[e], fmt, flags)) { atomic_fetch_add(&vr->errors, 1); return STATUS_UNSUCCESSFUL; }
  }
  int img[2];
  for (int e = 0; e < 2; e++) {
    img[e] = -1;
    for (int i = 0; i < VR_IMAGES && img[e] < 0; i++) {
      int expect = 0;
      if (atomic_compare_exchange_strong(&vr->sc[e].busy[i], &expect, 1)) img[e] = i;
    }
  }
  if (img[0] < 0 || img[1] < 0) {
    for (int e = 0; e < 2; e++) if (img[e] >= 0) atomic_store(&vr->sc[e].busy[img[e]], 0);
    atomic_fetch_add(&vr->dropped, 1);
    return STATUS_SUCCESS;
  }
  struct vrl_submit p;
  memset(&p, 0, sizeof p);
  p.h.type = VRL_SUBMIT; p.h.size = sizeof p;
  /* back to the host's pacer id (wmtr_vr_frame_cadence offset); a locally paced frame maps to an id the pacer
   * does not know, which it ignores */
  pthread_mutex_lock(&vr->lock);
  int64_t host_id = (int64_t)info->frame_id - vr->cadence.offset;
  pthread_mutex_unlock(&vr->lock);
  p.frame_id = host_id > 0 ? (uint64_t)host_id : info->frame_id;
  p.display_time = (int64_t)info->display_time;
  p.app_submit_time = t_submit;
  memcpy(p.pose, info->pose, sizeof p.pose);
  memcpy(p.fov, info->fov, sizeof p.fov);
  id<MTLCommandBuffer> cb = [vr->queue commandBuffer];
  uint64_t wait_event = info->wait_event, wait_value = info->wait_value;
  if (wait_event && wait_value) [cb encodeWaitForEvent:(id<MTLSharedEvent>)wait_event value:wait_value];
  id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
  for (int e = 0; e < 2; e++) {
    [be copyFromTexture:src[e] sourceSlice:info->eye[e].slice sourceLevel:0 sourceOrigin:MTLOriginMake(rx[e], ry[e], 0)
             sourceSize:MTLSizeMake(rw[e], rh[e], 1) toTexture:vr->sc[e].image[img[e]] destinationSlice:0
       destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    p.eye[e] = (struct vrl_eye){vr->sc[e].id, (uint32_t)img[e], 0, 0, rw[e], rh[e],
                                (info->eye[e].flags & WMTVR_EYE_FLIP_Y) ? VRL_EYE_FLIP_Y : 0, 0};
  }
  [be endEncoding];
  atomic_fetch_add(&vr->inflight, 1);
  [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
    struct vrl_submit q = p;
    if (done.status != MTLCommandBufferStatusCompleted) {
      atomic_fetch_add(&vr->errors, 1);
      for (int e = 0; e < 2; e++)
        if (vr->sc[e].id == q.eye[e].swapchain) atomic_store(&vr->sc[e].busy[q.eye[e].image], 0);
      atomic_fetch_sub(&vr->inflight, 1);
      return;
    }
    q.ready_time = now_ns();
    stage_account(vr, q.app_submit_time, wait_event && wait_value ? signal_time(vr, wait_event, wait_value) : 0, done, q.ready_time);
    /* Never block Metal's completion thread on the link (a stalled host would stall every completion handler of
     * the process, the game's too): when the host's buffer is full the frame is dropped and its images freed. */
    ssize_t r;
    do r = send(vr->fd, &q, sizeof q, MSG_DONTWAIT); while (r < 0 && errno == EINTR);
    if (r == (ssize_t)sizeof q) atomic_fetch_add(&vr->sent, 1);
    else {
      if (r < 0 && (errno == EAGAIN || errno == ENOBUFS)) atomic_fetch_add(&vr->dropped, 1);
      else atomic_fetch_add(&vr->errors, 1);
      for (int e = 0; e < 2; e++)
        if (vr->sc[e].id == q.eye[e].swapchain) atomic_store(&vr->sc[e].busy[q.eye[e].image], 0);
    }
    atomic_fetch_sub(&vr->inflight, 1);
  }];
  [cb commit];
  return STATUS_SUCCESS;
}

struct wmtr_vr *
wmtr_vr_create(int link_fd, const char *mach_name) {
  if (link_fd < 0 || !mach_name) return NULL;
  struct wmtr_vr *vr = calloc(1, sizeof *vr);
  vr->fd = link_fd;
  vr->sig_lock = OS_UNFAIR_LOCK_INIT;
  snprintf(vr->mach_name, sizeof vr->mach_name, "%s", mach_name);
  int on = 1;
  setsockopt(link_fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
  pthread_mutexattr_t ma;
  pthread_mutexattr_init(&ma);
  pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&vr->lock, &ma);
  pthread_mutexattr_destroy(&ma);
  /* the host sends VRL_TIMING/VRL_POSES every frame and they are read only when the guest calls in; the default
   * AF_UNIX datagram space (net.local.dgram.recvspace, 4 KiB) fills in a few frames and would block the host's
   * VRL_RELEASE sends behind them */
  int space = VRL_SOCKET_BUFFER;
  setsockopt(link_fd, SOL_SOCKET, SO_RCVBUF, &space, sizeof space);
  /* WMTR_VR_FD outlives its links: while no context was attached the host kept sending (timing and poses every
   * frame) and the socket is full of packets for nobody. They are dropped here, before the hello: the host's answer
   * (its state) is sent without waiting and would be lost against the full buffer. */
  uint8_t stale[2048];
  for (int i = 0; i < 65536 && recv(link_fd, stale, sizeof stale, MSG_DONTWAIT) > 0; i++) {}
  struct vrl_hello hello = {{VRL_HELLO, sizeof hello}, VRL_VERSION, (uint32_t)getpid()};
  send_packet(vr, &hello, sizeof hello);
  pthread_create(&vr->reader, NULL, reader_main, vr);
  return vr;
}

struct wmtr_vr *
wmtr_vr_connect(const char *path, const char *mach_name) {
  /* the listener (steamac-vrhost, or a test) answers with one end of a SOCK_DGRAM socketpair (SCM_RIGHTS):
     macOS has no AF_UNIX SOCK_SEQPACKET */
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0) return NULL;
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
  int fd = -1;
  if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
    char byte;
    struct iovec iov = {&byte, 1};
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } c;
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = c.buf, .msg_controllen = sizeof c.buf};
    if (recvmsg(s, &m, 0) == 1) {
      struct cmsghdr *ch = CMSG_FIRSTHDR(&m);
      if (ch && ch->cmsg_level == SOL_SOCKET && ch->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(ch), sizeof fd);
    }
  }
  close(s);
  if (fd < 0) return NULL;
  struct wmtr_vr *vr = wmtr_vr_create(fd, mach_name);
  if (!vr) close(fd);
  return vr;
}

void
wmtr_vr_destroy(struct wmtr_vr *vr) {
  if (!vr) return;
  atomic_store(&vr->quit, 1);
  pthread_join(vr->reader, NULL);
  /* completion handlers reference vr; a bound keeps destroy from hanging on a wedged GPU */
  for (int i = 0; i < 2000 && atomic_load(&vr->inflight); i++) usleep(1000);
  [vr->queue release];
  close(vr->fd);
  for (int e = 0; e < 2; e++) swapchain_free(&vr->sc[e]);
  if (vr->service) mach_port_deallocate(mach_task_self(), vr->service);
  [vr->device release];
  pthread_mutex_destroy(&vr->lock);
  free(vr);
}

void
wmtr_vr_get_stats(struct wmtr_vr *vr, struct wmtr_vr_stats *o) {
  o->submits = vr->submits; o->sent = vr->sent; o->dropped = vr->dropped;
  o->released = vr->released; o->errors = vr->errors; o->swapchains = vr->swapchains;
  o->queries = vr->queries; o->haptics = vr->haptics;
  o->stage_frames = vr->stage_frames;
  for (int i = 0; i < WMTR_VR_STAGES; i++) o->stage_ns[i] = vr->stage_ns[i];
}
