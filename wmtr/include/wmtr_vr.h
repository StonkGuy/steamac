/* wmtr VR export (vm side of the VR link, docs/vr-interfaces.md section 3).
 * WMTVRSubmit (winemetal call 146) arrives on the executor thread with the guest's eye textures (host
 * MTLTexture handles from DXMT's IMTLD3D11ExportableTexture). The exporter copies each eye into an
 * IOSurface-backed swapchain image shared with steamac-vrhost (one GPU blit per eye, ordered after the
 * guest's rendering through the DXMT shared event), and sends VRL_SUBMIT when the copy has completed.
 * It never blocks the executor: a frame with no free swapchain image is dropped (counted). */
#ifndef WMTR_VR_H
#define WMTR_VR_H
#include <stdint.h>

struct wmtr_vr;
struct WMTVRSubmitInfo;
struct wmtr_host;

/* link_fd: one end of an AF_UNIX SOCK_DGRAM socketpair to steamac-vrhost (ownership passes to the exporter).
 * mach_name: bootstrap name under which the host receives swapchain ports. NULL on failure. */
struct wmtr_vr *wmtr_vr_create(int link_fd, const char *mach_name);
/* connect to a listener at `path` (SOCK_STREAM) that hands over the link fd by SCM_RIGHTS (tests, manual runs) */
struct wmtr_vr *wmtr_vr_connect(const char *path, const char *mach_name);
void wmtr_vr_destroy(struct wmtr_vr *vr);
/* returns an NTSTATUS (0 = queued or dropped, see stats) */
int wmtr_vr_submit(struct wmtr_vr *vr, const struct WMTVRSubmitInfo *info);

/* WMTR_CTL_VR_QUERY: `buf` holds one request of the guest VR protocol (guest/vr/include/steamac_vr_guest.h:
 * SVR_HELLO, SVR_FRAME, SVR_HAPTIC) in `size` bytes and is overwritten with the reply. Answered from the newest
 * VRL_STATE/VRL_TIMING/VRL_POSES/VRL_INPUT the host sent, without waiting; SVR_HAPTIC becomes VRL_HAPTICS.
 * vr == NULL (no VR host attached) answers HELLO with connected = 0. Returns an NTSTATUS. */
int wmtr_vr_query(struct wmtr_vr *vr, void *buf, uint32_t size);

/* Where a frame's time goes between its WMTVRSubmit and its VRL_SUBMIT (vrhost's "submit->copied"); the four
 * stages add up to it. The submit overtakes the frame's own rendering: DXMT's encode thread is still sending that
 * rendering (and what is queued before it, up to dxgi.maxFrameLatency frames) when the submit arrives. */
enum {
  WMTR_VR_STAGE_ENCODE, /* submit -> the executor encoded the event signal that ends the frame's rendering */
  WMTR_VR_STAGE_RENDER, /* -> the GPU finished that rendering (the copy starts) */
  WMTR_VR_STAGE_COPY,   /* the eye copy on the GPU */
  WMTR_VR_STAGE_NOTIFY, /* -> completion handler (VRL_SUBMIT sent) */
  WMTR_VR_STAGES
};

struct wmtr_vr_stats {
  uint64_t submits;   /* WMTVRSubmit calls */
  uint64_t sent;      /* VRL_SUBMIT packets sent */
  uint64_t dropped;   /* no free swapchain image */
  uint64_t released;  /* VRL_RELEASE received */
  uint64_t errors;    /* bad textures, unsupported formats, link errors */
  uint64_t swapchains;/* swapchains (re)created */
  uint64_t queries;   /* WMTR_CTL_VR_QUERY requests answered */
  uint64_t haptics;   /* VRL_HAPTICS sent */
  uint64_t stage_frames;             /* frames in stage_ns */
  uint64_t stage_ns[WMTR_VR_STAGES]; /* cumulative, see WMTR_VR_STAGE_* */
};
void wmtr_vr_get_stats(struct wmtr_vr *vr, struct wmtr_vr_stats *out);

/* The frame the guest's WaitGetPoses gets (SVR_FRAME), from the host's newest pacer timing. The guest's frame ids
 * must only grow, but the host's restart at every headset (re)connect (pacer reset) and stop while none is
 * connected; ids are mapped (host id + offset) and, without a fresh timing, the cadence continues locally from
 * `now`. The wake-up time is never more than two frames ahead: a guest must not sleep on a stale or remapped
 * timing (2026-10-17: a reconnect mid-game put it ~9000 frames ahead, VRChat froze). Pure, for tests. */
/* While the pacer runs (its newest timing is current), a local frame is only what the guest gets when it asks before
 * the pacer's next frame is out: its wake-up is when that frame is due (last host wake-up + render_period, the
 * pacer's interval: spacewarp releases a frame every other display period, and the host may hold one while the GPU
 * is behind), and the flags tell the guest to wait for it rather than render a frame the headset never asked for.
 * Returns WMTR_VR_FRAME_* (= SVR_FRAME_*). */
#define WMTR_VR_FRAME_FROM_PACER 1u /* a fresh timing: the pacer's own frame */
#define WMTR_VR_FRAME_PACER_LIVE 2u /* the pacer's newest timing is current */
/* how long after its last timing the pacer counts as running: in time, not display periods, since a host that holds
 * frames for a GPU-bound game (VRChat at 24 fps: 41 ms+ per frame) sends its timings several periods apart */
#define WMTR_VR_PACER_LIVE_NS 150000000ll
struct wmtr_vr_cadence {
  uint64_t host_last;  /* host frame id of the last fresh timing used */
  int64_t offset;      /* guest id = host id + offset */
  int64_t last_wake;   /* wake-up of the last frame handed out */
};
unsigned wmtr_vr_frame_cadence(struct wmtr_vr_cadence *c, uint64_t host_id, int64_t host_wake, int64_t host_display,
                               uint64_t guest_last, int64_t now, int64_t period, int64_t render_period, uint64_t *id,
                               int64_t *wake, int64_t *display);
/* the executor encoded a signal of shared event `event` (host object) to `value` for the guest: the time is kept
 * for the stage split of the frames that wait for it. vr == NULL is ignored. */
void wmtr_vr_note_signal(struct wmtr_vr *vr, uint64_t event, uint64_t value);

/* attach an exporter to an executor (call 146); NULL detaches. Without one, call 146 fails with
 * STATUS_NOT_SUPPORTED. wmtr_host_create attaches one itself when WMTR_VR_MACH_NAME is set together with
 * WMTR_VR_FD (a socketpair end open in this process, as steamac-vm passes it; shared by live contexts)
 * or WMTR_VR_SOCKET (a listener path, tests and manual runs). */
void wmtr_host_set_vr(struct wmtr_host *h, struct wmtr_vr *vr);
/* the context's VR link (WMTR_VR_FD: the process-wide shared one), or NULL */
struct wmtr_vr *wmtr_host_vr(struct wmtr_host *h);

#endif
