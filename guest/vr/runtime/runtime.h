// SPDX-License-Identifier: Apache-2.0
// Process-wide runtime state behind the OpenVR interfaces: host link, frame clock, poses, events, submits.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "openvr.h"
#include "steamac_vr_guest.h"
#include "transport.h"

namespace svr {
// OpenVR tracked device indices this runtime exposes
enum : vr::TrackedDeviceIndex_t { kHmd = 0, kLeft = 1, kRight = 2, kDeviceCount = 3 };

int64_t now_ns();  // guest CLOCK_MONOTONIC

class Runtime {
 public:
  static Runtime &get();

  vr::EVRInitError init(vr::EVRApplicationType type);
  void shutdown();
  bool ready() const { return transport_ != nullptr; }

  // headset description (from SVR_HELLO, refreshed by SVR_FRAME)
  svr_hello_rep hello() const;
  float refresh_hz() const;

  // --- frame clock ---
  // WaitGetPoses: flush a pending submit, then block until the host's wake_up of the next frame and
  // fetch its predicted poses. Returns false if the host is gone.
  bool wait_frame();
  // latest snapshot without waiting (GetDeviceToAbsoluteTrackingPose, input polls)
  bool poll();
  // poses for the current frame (from the last wait_frame), and the frame's guest display time
  svr_pose frame_pose(int dev) const;
  int64_t frame_display_guest() const;
  uint64_t frame_id() const;
  // pose of a device at guest time t (extrapolated from the latest snapshot)
  svr_pose pose_at(int dev, int64_t guest_t) const;
  // OpenVR device pose (device index) predicted `seconds_from_now`, or for the current frame
  vr::TrackedDevicePose_t device_pose(vr::TrackedDeviceIndex_t i, float seconds_from_now) const;
  vr::TrackedDevicePose_t device_frame_pose(vr::TrackedDeviceIndex_t i) const;
  bool device_connected(vr::TrackedDeviceIndex_t i) const;
  int64_t last_vsync_guest() const;
  uint32_t frames_presented() const { return frames_presented_; }

  // Which pose APIs the app uses and what they answered, summarised in one info line every ~5 s (a live run shows
  // whether e.g. the head pose the app reads is valid and moving). Relaxed counters: no cost beyond an increment.
  struct PoseStats {
    std::atomic<uint32_t> wait{0}, wait_fail{0}, last_poses{0}, last_pose_index{0}, abs_pose{0}, abs_seated{0},
        pose_action{0}, pose_action_inactive{0}, connected_query{0}, connected_false_hmd{0};
  };
  PoseStats &pose_stats() { return pose_stats_; }
  void note_render_head(const vr::TrackedDevicePose_t &p);

  // input snapshot (the frame's)
  float input(int hand, int id) const;
  uint32_t profile() const;

  // --- submit ---
  // array_index: SubmitWithArrayIndex's explicit slice, or ~0u (Submit: use metadata slice for arrays).
  vr::EVRCompositorError submit(vr::EVREye eye, const vr::Texture_t *tex, const vr::VRTextureBounds_t *bounds,
                                vr::EVRSubmitFlags flags, uint32_t array_index = ~0u);
  void post_present_handoff();  // sends the pending frame now
  void clear_frame();

  // --- haptics ---
  void haptic(int hand, float duration_s, float frequency, float amplitude);

  // --- events ---
  void push_event(vr::EVREventType type, vr::TrackedDeviceIndex_t dev);
  bool pop_event(vr::VREvent_t *ev, uint32_t size);

  std::string app_key;

 private:
  Runtime() = default;
  bool fetch(svr_frame_rep *out);
  void flush_submit_locked();
  int64_t host_to_guest(int64_t host_t) const { return host_t + clock_offset_; }

  mutable std::mutex m_;
  std::unique_ptr<Transport> transport_;
  svr_hello_rep hello_{};
  bool hello_view_ = false;  // hello_ carries a connected headset's eye size / FOV / IPD
  svr_frame_rep latest_{};  // newest snapshot
  svr_frame_rep frame_{};   // snapshot of the frame being rendered
  int64_t clock_offset_ = 0;  // guest = host + offset
  int64_t last_vsync_ = 0;
  uint32_t frames_presented_ = 0;
  PoseStats pose_stats_;
  int64_t stats_logged_ = 0;
  float stats_head_min_[3] = {1e9f, 1e9f, 1e9f}, stats_head_max_[3] = {-1e9f, -1e9f, -1e9f};
  uint32_t stats_head_invalid_ = 0;
  svr_wmtvr_submit pending_{};
  bool pending_eye_[2] = {};
  // diagnostics: the last logged submit geometry per eye and head pose flags (logged when they change)
  struct SubmitGeometry {
    uint32_t width, height, format, array_size, slice, x, y, w, h, flags;
  };
  SubmitGeometry logged_geometry_[2] = {};
  uint32_t logged_head_flags_ = ~0u;
  bool pending_valid_ = false;
  std::deque<vr::VREvent_t> events_;
  bool connected_[kDeviceCount] = {};
};
}  // namespace svr
