/* SPDX-License-Identifier: Apache-2.0
 *
 * vr-fakehost: a headset-less stand-in for the steamac VR host on the guest link (include/steamac_vr_guest.h),
 * socket transport. It paces frames at a fixed refresh rate, serves poses and inputs from a synthetic motion
 * model plus an optional script (recorded or hand-written), and checks and logs every submit and haptic event.
 *
 *   vr-fakehost --socket PATH [--script FILE] [--log FILE] [--hz 90] [--profile touch|index] [--once] [--live-view]
 *
 * Script lines (frame-keyed, so a replay is deterministic whatever the client's frame rate):
 *   <frame> in <L|R> <input-name> <value>          e.g.  20 in R TRIGGER_VALUE 1
 *   <frame> pose <dev> qx qy qz qw px py pz        dev: HEAD LEFT_GRIP LEFT_AIM RIGHT_GRIP RIGHT_AIM
 *   <frame> velocity <dev> vx vy vz wx wy wz
 *   <frame> disconnect | connect
 * An input keeps its value until changed. A scripted pose replaces the synthetic one from that frame on.
 * Log lines (JSON): {"ev":"hello"...}, {"ev":"submit",...,"ok":true}, {"ev":"haptic",...}, {"ev":"summary",...}.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "steamac_vr_guest.h"

static const char *dev_names[SVR_DEV_COUNT] = {"HEAD", "LEFT_GRIP", "LEFT_AIM", "RIGHT_GRIP", "RIGHT_AIM"};
static const char *in_names[SVR_IN_COUNT] = {
    "TRIGGER_VALUE", "TRIGGER_CLICK", "TRIGGER_TOUCH",  "SQUEEZE_VALUE",  "SQUEEZE_CLICK",  "SQUEEZE_FORCE",
    "THUMBSTICK_X",  "THUMBSTICK_Y",  "THUMBSTICK_CLICK", "THUMBSTICK_TOUCH", "A_CLICK",     "A_TOUCH",
    "B_CLICK",       "B_TOUCH",       "MENU_CLICK",     "SYSTEM_CLICK",   "THUMBREST_TOUCH", "TRACKPAD_X",
    "TRACKPAD_Y",    "TRACKPAD_TOUCH", "TRACKPAD_FORCE"};

enum { EV_IN, EV_POSE, EV_VEL, EV_CONN };
struct event {
  uint64_t frame;
  int kind, hand, id, dev;
  float v[7];
};

static struct event *events;
static size_t n_events;
static FILE *logf_;
static double hz = 90;
static uint32_t profile = SVR_PROFILE_TOUCH;
static int64_t t0;

/* per-frame issued info, to check submits against */
#define HIST 256
static struct {
  uint64_t frame;
  int64_t display;
  struct svr_pose head;
} hist[HIST];

static struct {
  unsigned frames, submits, submit_errors, haptics, hellos;
} stats;

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int find_name(const char *const *names, int n, const char *s) {
  for (int i = 0; i < n; i++)
    if (!strcmp(names[i], s)) return i;
  return -1;
}

static int load_script(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  char line[512];
  int ln = 0;
  while (fgets(line, sizeof(line), f)) {
    ln++;
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\n' || !*p) continue;
    struct event e = {0};
    char kind[32] = "", a[64] = "", b[64] = "";
    unsigned long long fr;
    if (sscanf(p, "%llu %31s", &fr, kind) != 2) goto bad;
    e.frame = fr;
    if (!strcmp(kind, "in")) {
      float v;
      if (sscanf(p, "%*u %*s %63s %63s %f", a, b, &v) != 3) goto bad;
      e.kind = EV_IN;
      e.hand = a[0] == 'R' || a[0] == 'r';
      e.id = find_name(in_names, SVR_IN_COUNT, b);
      e.v[0] = v;
      if (e.id < 0) goto bad;
    } else if (!strcmp(kind, "pose") || !strcmp(kind, "velocity")) {
      int want = !strcmp(kind, "pose") ? 7 : 6;
      e.kind = want == 7 ? EV_POSE : EV_VEL;
      if (sscanf(p, "%*u %*s %63s %f %f %f %f %f %f %f", a, &e.v[0], &e.v[1], &e.v[2], &e.v[3], &e.v[4], &e.v[5],
                 &e.v[6]) < 1 + want)
        goto bad;
      e.dev = find_name(dev_names, SVR_DEV_COUNT, a);
      if (e.dev < 0) goto bad;
    } else if (!strcmp(kind, "disconnect") || !strcmp(kind, "connect")) {
      e.kind = EV_CONN;
      e.v[0] = !strcmp(kind, "connect");
    } else {
      goto bad;
    }
    events = realloc(events, (n_events + 1) * sizeof(*events));
    events[n_events++] = e;
    continue;
  bad:
    fprintf(stderr, "vr-fakehost: %s:%d: cannot parse: %s", path, ln, line);
    fclose(f);
    return -1;
  }
  fclose(f);
  /* insertion sort by frame, stable: lines of one frame keep their order */
  for (size_t i = 1; i < n_events; i++)
    for (size_t j = i; j > 0 && events[j - 1].frame > events[j].frame; j--) {
      struct event t = events[j];
      events[j] = events[j - 1], events[j - 1] = t;
    }
  return 0;
}

static void quat_yaw(float yaw, float q[4]) {
  q[0] = 0, q[1] = sinf(yaw / 2), q[2] = 0, q[3] = cosf(yaw / 2);
}

/* synthetic motion at time t (seconds since start): head yaws slowly, hands bob */
#define HEAD_YAW_RATE 0.5f
static void synthetic(double t, struct svr_pose pose[SVR_DEV_COUNT]) {
  memset(pose, 0, sizeof(struct svr_pose) * SVR_DEV_COUNT);
  for (int d = 0; d < SVR_DEV_COUNT; d++) {
    pose[d].flags = SVR_POSE_VALID | SVR_POSE_TRACKED | SVR_POSE_VEL;
    quat_yaw(0, pose[d].orientation);
  }
  quat_yaw(HEAD_YAW_RATE * (float)t, pose[SVR_DEV_HEAD].orientation);
  pose[SVR_DEV_HEAD].position[1] = 1.7f;
  pose[SVR_DEV_HEAD].ang_vel[1] = HEAD_YAW_RATE;
  for (int h = 0; h < 2; h++) {
    float x = h ? 0.2f : -0.2f, bob = 0.05f * sinf((float)t * 2.0f), vel = 0.1f * cosf((float)t * 2.0f);
    struct svr_pose *g = &pose[h ? SVR_DEV_RIGHT_GRIP : SVR_DEV_LEFT_GRIP], *a = &pose[h ? SVR_DEV_RIGHT_AIM : SVR_DEV_LEFT_AIM];
    g->position[0] = x, g->position[1] = 1.2f + bob, g->position[2] = -0.3f, g->lin_vel[1] = vel;
    *a = *g;
    a->position[2] = -0.35f;  /* aim sits a little in front of the grip */
  }
}

/* state at frame f: synthetic poses at display time, then script events up to f */
static void state_at(uint64_t f, double t, struct svr_frame_rep *r) {
  synthetic(t, r->pose);
  memset(r->input, 0, sizeof(r->input));
  r->connected = 1;
  static struct svr_pose scripted[SVR_DEV_COUNT];
  static int has_scripted[SVR_DEV_COUNT];
  memset(has_scripted, 0, sizeof(has_scripted));
  for (size_t i = 0; i < n_events && events[i].frame <= f; i++) {
    struct event *e = &events[i];
    if (e->kind == EV_IN) {
      r->input[e->hand][e->id] = e->v[0];
    } else if (e->kind == EV_POSE) {
      struct svr_pose *p = &scripted[e->dev];
      memset(p, 0, sizeof(*p));
      memcpy(p->orientation, e->v, 4 * sizeof(float));
      memcpy(p->position, e->v + 4, 3 * sizeof(float));
      p->flags = SVR_POSE_VALID | SVR_POSE_TRACKED;
      has_scripted[e->dev] = 1;
    } else if (e->kind == EV_VEL) {
      struct svr_pose *p = &scripted[e->dev];
      memcpy(p->lin_vel, e->v, 3 * sizeof(float));
      memcpy(p->ang_vel, e->v + 3, 3 * sizeof(float));
      p->flags |= SVR_POSE_VEL;
    } else if (e->kind == EV_CONN) {
      r->connected = e->v[0] != 0;
    }
  }
  for (int d = 0; d < SVR_DEV_COUNT; d++)
    if (has_scripted[d]) r->pose[d] = scripted[d];
  if (!r->connected)
    for (int d = 0; d < SVR_DEV_COUNT; d++) r->pose[d].flags = 0;
}

static void fill_view(float fov[2][4], float *ipd) {
  for (int e = 0; e < 2; e++) {
    /* Quest-3-like asymmetric FOV, mirrored per eye */
    fov[e][0] = e ? -0.785f : -0.942f;
    fov[e][1] = e ? 0.942f : 0.785f;
    fov[e][2] = 0.873f;
    fov[e][3] = -0.873f;
  }
  *ipd = 0.064f;
}

/* --live-view: the frames carry a different view than the hello did (a game that started on the host's standby
 * view, then a headset with its own FOV and IPD connected). The game renders with what the hello reported
 * (GetProjectionRaw, GetEyeToHeadTransform), so its submits must still carry the hello's view. */
static int live_view;
static void fill_frame_view(float fov[2][4], float *ipd) {
  fill_view(fov, ipd);
  if (!live_view) return;
  for (int e = 0; e < 2; e++) {
    fov[e][0] = e ? -0.698f : -0.942f; /* Meta Quest 3 under WiVRn v26.9 */
    fov[e][1] = e ? 0.942f : 0.698f;
    fov[e][2] = 0.768f;
    fov[e][3] = -0.960f;
  }
  *ipd = 0.068f;
}

static int handle(int fd, const void *buf, size_t n) {
  const struct svr_hdr *h = buf;
  if (n < sizeof(*h) || h->size != n) return -1;
  int64_t period = (int64_t)(1e9 / hz);
  switch (h->op) {
    case SVR_HELLO: {
      const struct svr_hello_req *q = buf;
      struct svr_hello_rep r = {{SVR_HELLO, sizeof(r)}};
      r.version = SVR_VERSION;
      r.connected = 1;
      r.eye_width = 1680, r.eye_height = 1760;
      r.refresh_hz = (float)hz;
      fill_view(r.fov, &r.ipd_m);
      r.profile = profile;
      snprintf(r.name, sizeof(r.name), "fakehost %s", profile == SVR_PROFILE_INDEX ? "Index" : "Touch");
      stats.hellos++;
      if (logf_) fprintf(logf_, "{\"ev\":\"hello\",\"version\":%u,\"pid\":%u,\"app\":\"%.64s\"}\n", q->version, q->pid, q->app_key);
      return send(fd, &r, sizeof(r), MSG_NOSIGNAL) == sizeof(r) ? 0 : -1;
    }
    case SVR_FRAME: {
      const struct svr_frame_req *q = buf;
      struct svr_frame_rep r = {{SVR_FRAME, sizeof(r)}};
      int64_t now = now_ns();
      /* the next frame after the client's last; a client that fell behind skips to the frame whose
       * wake-up is the latest one already passed (frame f wakes at t0 + f * period) */
      uint64_t cur = (uint64_t)((now - t0) / period);
      uint64_t f = q->last_frame_id + 1 > cur ? q->last_frame_id + 1 : cur;
      r.frame_id = f;
      r.predicted_display = t0 + (int64_t)f * period + 2 * period;
      r.wake_up = r.predicted_display - 2 * period;
      r.pose_time = r.predicted_display;
      r.host_now = now;
      r.refresh_hz = (float)hz;
      r.profile = profile;
      fill_frame_view(r.fov, &r.ipd_m);
      state_at(f, (r.predicted_display - t0) * 1e-9, &r);
      hist[f % HIST].frame = f;
      hist[f % HIST].display = r.predicted_display;
      hist[f % HIST].head = r.pose[SVR_DEV_HEAD];
      if (f > stats.frames) stats.frames = (unsigned)f;
      return send(fd, &r, sizeof(r), MSG_NOSIGNAL) == sizeof(r) ? 0 : -1;
    }
    case SVR_SUBMIT: {
      const struct svr_wmtvr_submit *q = &((const struct svr_submit_req *)buf)->info;
      const char *why = NULL;
      uint64_t f = q->frame_id;
      if (hist[f % HIST].frame != f) why = "unknown frame";
      else if (hist[f % HIST].display != (int64_t)q->display_time) why = "display time differs from the frame's";
      else if (!q->eye[0].texture || !q->eye[1].texture) why = "missing eye";
      else {
        /* eye poses: the frame's head orientation; positions straddle the head position by the IPD */
        const struct svr_pose *hp = &hist[f % HIST].head;
        float ipd2 = 0, mid[3];
        for (int i = 0; i < 3; i++) {
          mid[i] = (q->pose[0][4 + i] + q->pose[1][4 + i]) / 2;
          float d = q->pose[1][4 + i] - q->pose[0][4 + i];
          ipd2 += d * d;
        }
        for (int i = 0; i < 4 && !why; i++)
          if (q->pose[0][i] != hp->orientation[i] || q->pose[1][i] != hp->orientation[i]) why = "eye orientation differs from the frame's head";
        for (int i = 0; i < 3 && !why; i++)
          if (fabsf(mid[i] - hp->position[i]) > 1e-5f) why = "eye positions not centred on the frame's head";
        float hello_fov[2][4], hello_ipd;
        fill_view(hello_fov, &hello_ipd);
        if (!why && fabsf(sqrtf(ipd2) - hello_ipd) > 1e-5f) why = "eye separation is not the reported IPD";
        for (int e = 0; e < 2 && !why; e++)
          for (int i = 0; i < 4; i++)
            if (q->fov[e][i] != hello_fov[e][i]) why = "eye FOV is not the reported projection";
      }
      stats.submits++;
      if (why) stats.submit_errors++;
      if (logf_)
        fprintf(logf_,
                "{\"ev\":\"submit\",\"frame\":%llu,\"texture\":[%llu,%llu],\"rect0\":[%u,%u,%u,%u],\"flags\":[%u,%u],"
                "\"slice\":[%u,%u],\"wait\":[%llu,%llu],\"ok\":%s%s%s%s}\n",
                (unsigned long long)f, (unsigned long long)q->eye[0].texture, (unsigned long long)q->eye[1].texture,
                q->eye[0].x, q->eye[0].y, q->eye[0].w, q->eye[0].h, q->eye[0].flags, q->eye[1].flags, q->eye[0].slice,
                q->eye[1].slice, (unsigned long long)q->wait_event, (unsigned long long)q->wait_value,
                why ? "false" : "true", why ? ",\"why\":\"" : "", why ? why : "", why ? "\"" : "");
      struct svr_ack a = {{SVR_SUBMIT, sizeof(a)}, why ? -EINVAL : 0, 0};
      return send(fd, &a, sizeof(a), MSG_NOSIGNAL) == sizeof(a) ? 0 : -1;
    }
    case SVR_HAPTIC: {
      const struct svr_haptic_req *q = buf;
      stats.haptics++;
      if (logf_)
        fprintf(logf_, "{\"ev\":\"haptic\",\"hand\":%u,\"duration\":%.4f,\"frequency\":%.1f,\"amplitude\":%.3f}\n", q->hand,
                q->duration_s, q->frequency, q->amplitude);
      struct svr_ack a = {{SVR_HAPTIC, sizeof(a)}, 0, 0};
      return send(fd, &a, sizeof(a), MSG_NOSIGNAL) == sizeof(a) ? 0 : -1;
    }
    default:
      return -1;
  }
}

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s, stop = 1; }

static void summary(void) {
  if (!logf_) return;
  fprintf(logf_, "{\"ev\":\"summary\",\"hellos\":%u,\"frames\":%u,\"submits\":%u,\"submit_errors\":%u,\"haptics\":%u}\n",
          stats.hellos, stats.frames, stats.submits, stats.submit_errors, stats.haptics);
  fflush(logf_);
}

int main(int argc, char **argv) {
  const char *sock = NULL, *script = NULL, *log = NULL;
  int once = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--socket") && i + 1 < argc) sock = argv[++i];
    else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
    else if (!strcmp(argv[i], "--log") && i + 1 < argc) log = argv[++i];
    else if (!strcmp(argv[i], "--hz") && i + 1 < argc) hz = atof(argv[++i]);
    else if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = !strcmp(argv[++i], "index") ? SVR_PROFILE_INDEX : SVR_PROFILE_TOUCH;
    else if (!strcmp(argv[i], "--once")) once = 1;
    else if (!strcmp(argv[i], "--live-view")) live_view = 1;
    else {
      fprintf(stderr, "usage: vr-fakehost --socket PATH [--script FILE] [--log FILE] [--hz N] [--profile touch|index] [--once] [--live-view]\n");
      return 2;
    }
  }
  if (!sock || hz <= 0) return fprintf(stderr, "vr-fakehost: --socket required\n"), 2;
  if (script && load_script(script)) return 1;
  if (log && !(logf_ = fopen(log, "w"))) return perror(log), 1;
  if (logf_) setvbuf(logf_, NULL, _IOLBF, 0);

  int ls = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  strncpy(a.sun_path, sock, sizeof(a.sun_path) - 1);
  unlink(sock);
  if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 4)) return perror("vr-fakehost: socket"), 1;
  struct sigaction sa = {.sa_handler = on_signal};
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  t0 = now_ns();
  fprintf(stderr, "vr-fakehost: listening on %s (%.0f Hz, %s, %zu script events)\n", sock, hz,
          profile == SVR_PROFILE_INDEX ? "index" : "touch", n_events);
  while (!stop) {
    int fd = accept(ls, NULL, NULL);
    if (fd < 0) {
      if (errno == EINTR) continue;
      break;
    }
    char buf[4096];
    ssize_t n;
    while (!stop && (n = recv(fd, buf, sizeof(buf), 0)) > 0)
      if (handle(fd, buf, (size_t)n)) {
        fprintf(stderr, "vr-fakehost: bad request (op %u, %zd bytes)\n", ((struct svr_hdr *)buf)->op, n);
        break;
      }
    close(fd);
    if (once) break;
  }
  summary();
  unlink(sock);
  return 0;
}
