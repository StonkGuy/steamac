// SPDX-License-Identifier: Apache-2.0
// svr-tracker-feed: a minimal tracker source for the steamac tracked-device table, and a table inspector.
// It is the reference for writing a source (docs/vr-trackers.md): claim a slot per device, write its pose at a steady
// rate, release it on exit. The poses follow the headset (from the scene app's mirror) with a fixed standing-body
// layout, which is enough to see trackers in a title's full-body calibration; without a running scene app the body
// stands at the origin.
//
//   svr-tracker-feed [--table PATH] [--count 1..8] [--hz N] [--seconds N] [--prefix SERIAL_PREFIX]
//   svr-tracker-feed --list [--table PATH]     print the table (mirror and devices)
//   svr-tracker-feed --clear [--table PATH]    free every slot (after a crashed source)
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "trackers.h"

namespace {
volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

int64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// a standing body relative to the head, in the head's yaw frame (x right, -z forward); y_abs >= 0 pins the height
struct BodyPoint {
  uint32_t role;
  float x, y, z;
  float y_abs;
};
const BodyPoint kBody[8] = {
    {SVR_TRK_ROLE_WAIST, 0.0f, -0.70f, 0.05f, -1},
    {SVR_TRK_ROLE_LEFT_FOOT, -0.12f, 0, 0.0f, 0.08f},
    {SVR_TRK_ROLE_RIGHT_FOOT, 0.12f, 0, 0.0f, 0.08f},
    {SVR_TRK_ROLE_CHEST, 0.0f, -0.35f, 0.05f, -1},
    {SVR_TRK_ROLE_LEFT_KNEE, -0.10f, 0, -0.05f, 0.50f},
    {SVR_TRK_ROLE_RIGHT_KNEE, 0.10f, 0, -0.05f, 0.50f},
    {SVR_TRK_ROLE_LEFT_ELBOW, -0.25f, -0.50f, 0.10f, -1},
    {SVR_TRK_ROLE_RIGHT_ELBOW, 0.25f, -0.50f, 0.10f, -1},
};

// the head's yaw (rotation about +y) from its orientation quaternion
float head_yaw(const float q[4]) {
  float x = q[0], y = q[1], z = q[2], w = q[3];
  // forward (-z) rotated by q, projected on the floor
  float fx = -2 * (x * z + y * w), fz = -(1 - 2 * (x * x + y * y));
  return atan2f(-fx, -fz);
}

svr_pose body_pose(const BodyPoint &b, const float head_pos[3], float yaw) {
  svr_pose p{};
  float c = cosf(yaw), s = sinf(yaw);
  // rotate (x, z) by yaw about +y
  p.position[0] = head_pos[0] + c * b.x + s * b.z;
  p.position[1] = b.y_abs >= 0 ? b.y_abs : head_pos[1] + b.y;
  p.position[2] = head_pos[2] - s * b.x + c * b.z;
  p.orientation[1] = sinf(yaw / 2), p.orientation[3] = cosf(yaw / 2);
  p.flags = SVR_POSE_VALID | SVR_POSE_TRACKED;
  return p;
}

int list(svr::TrackerTable &t) {
  svr_trk_mirror m{};
  uint32_t s = t.read_mirror(&m);
  printf("mirror: seq %u connected %u profile %u %.0f Hz head %.3f %.3f %.3f flags %u\n", s, m.connected, m.profile,
         m.refresh_hz, m.pose[SVR_DEV_HEAD].position[0], m.pose[SVR_DEV_HEAD].position[1],
         m.pose[SVR_DEV_HEAD].position[2], m.pose[SVR_DEV_HEAD].flags);
  for (int i = 0; i < SVR_TRK_MAX; i++) {
    svr_trk_device d;
    if (!t.read_device(i, &d)) continue;
    printf("slot %d (index %d): %s class %u role %s type %s seq %u pid %u pos %.3f %.3f %.3f flags %u\n", i, 3 + i,
           d.serial, d.device_class, svr::tracker_role_setting(d.role), d.controller_type[0] ? d.controller_type : "-",
           d.seq, d.writer_pid, d.pose.position[0], d.pose.position[1], d.pose.position[2], d.pose.flags);
  }
  return 0;
}
}  // namespace

int main(int argc, char **argv) {
  std::string path, prefix = "steamac-feed";
  int count = 3;
  double hz = 60, seconds = 0;
  bool do_list = false, do_clear = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--table" && i + 1 < argc) path = argv[++i];
    else if (a == "--count" && i + 1 < argc) count = atoi(argv[++i]);
    else if (a == "--hz" && i + 1 < argc) hz = atof(argv[++i]);
    else if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
    else if (a == "--prefix" && i + 1 < argc) prefix = argv[++i];
    else if (a == "--list") do_list = true;
    else if (a == "--clear") do_clear = true;
    else {
      fprintf(stderr, "usage: svr-tracker-feed [--table PATH] [--count 1..8] [--hz N] [--seconds N] [--prefix S] | --list | --clear\n");
      return 2;
    }
  }
  if (count < 1 || count > 8 || hz <= 0) return fprintf(stderr, "svr-tracker-feed: bad --count/--hz\n"), 2;
  svr::TrackerTable t;
  if (!t.open(path.empty() ? nullptr : path.c_str(), !do_list)) {
    fprintf(stderr, "svr-tracker-feed: cannot open %s\n", path.empty() ? svr::TrackerTable::default_path().c_str() : path.c_str());
    return 1;
  }
  if (do_list) return list(t);
  if (do_clear) {
    for (int i = 0; i < SVR_TRK_MAX; i++) t.release(i);
    return 0;
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  int slot[8];
  for (int i = 0; i < count; i++) {
    svr_trk_device desc{};
    snprintf(desc.serial, sizeof desc.serial, "%s-%s", prefix.c_str(), svr::tracker_role_name(kBody[i].role));
    desc.role = kBody[i].role;
    desc.writer_pid = (uint32_t)getpid();
    slot[i] = t.claim(desc);
    if (slot[i] < 0) return fprintf(stderr, "svr-tracker-feed: table full\n"), 1;
    fprintf(stderr, "svr-tracker-feed: %s -> slot %d (device index %d)\n", desc.serial, slot[i], 3 + slot[i]);
  }
  svr::TrackerView view;
  int64_t start = now_ns(), period = (int64_t)(1e9 / hz);
  for (int64_t next = start; !g_stop && (seconds <= 0 || now_ns() - start < (int64_t)(seconds * 1e9)); next += period) {
    int64_t now = now_ns();
    svr_trk_mirror m{};
    float head[3] = {0, 1.7f, 0}, yaw = 0;
    if (view.mirror_live(t, now, &m) && (m.pose[SVR_DEV_HEAD].flags & SVR_POSE_VALID)) {
      memcpy(head, m.pose[SVR_DEV_HEAD].position, sizeof head);
      yaw = head_yaw(m.pose[SVR_DEV_HEAD].orientation);
    }
    for (int i = 0; i < count; i++) t.update(slot[i], body_pose(kBody[i], head, yaw), now);
    int64_t wait = next + period - now_ns();
    if (wait > 0) {
      timespec ts{(time_t)(wait / 1000000000), (long)(wait % 1000000000)};
      nanosleep(&ts, nullptr);
    }
  }
  for (int i = 0; i < count; i++) t.release(slot[i]);
  fprintf(stderr, "svr-tracker-feed: released %d devices\n", count);
  return 0;
}
