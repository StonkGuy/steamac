// SPDX-License-Identifier: Apache-2.0
#include "runtime.h"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "vrmath.h"

namespace svr {
int64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void sleep_until(int64_t t) {
  timespec ts{(time_t)(t / 1000000000), (long)(t % 1000000000)};
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
  }
}

Runtime &Runtime::get() {
  static Runtime *rt = new Runtime();  // never destroyed: the app may call into it from atexit handlers
  return *rt;
}

vr::EVRInitError Runtime::init(vr::EVRApplicationType type) {
  std::lock_guard<std::mutex> g(m_);
  if (transport_) return vr::VRInitError_None;
  transport_ = Transport::open_default();
  if (!transport_) {
    SVR_ERR("no host link (DXMT/wmtr not loaded in this process and STEAMAC_VR_SOCKET not set)");
    return vr::VRInitError_Init_HmdNotFound;
  }
  svr_hello_req req{};
  req.h = {SVR_HELLO, sizeof(req)};
  req.version = SVR_VERSION;
  req.pid = (uint32_t)getpid();
  strncpy(req.app_key, app_key.c_str(), sizeof(req.app_key) - 1);
  svr_hello_rep rep{};
  int r = transport_->call(&req.h, &rep.h, sizeof(rep));
  if (r || rep.h.op != SVR_HELLO || rep.version != SVR_VERSION) {
    SVR_ERR("host hello failed (%d, version %u)", r, rep.version);
    transport_.reset();
    return vr::VRInitError_Init_HmdNotFound;
  }
  hello_ = rep;
  SVR_INFO("init app type %d over %s: %s %ux%u per eye %.0f Hz connected=%u", (int)type, transport_->name(),
           rep.name, rep.eye_width, rep.eye_height, rep.refresh_hz, rep.connected);
  return vr::VRInitError_None;
}

void Runtime::shutdown() {
  std::lock_guard<std::mutex> g(m_);
  if (pending_valid_) flush_submit_locked();
  transport_.reset();
  events_.clear();
  memset(connected_, 0, sizeof(connected_));
}

svr_hello_rep Runtime::hello() const {
  std::lock_guard<std::mutex> g(m_);
  return hello_;
}

float Runtime::refresh_hz() const {
  std::lock_guard<std::mutex> g(m_);
  return latest_.refresh_hz > 0 ? latest_.refresh_hz : hello_.refresh_hz > 0 ? hello_.refresh_hz : 90.0f;
}

// caller holds no lock
bool Runtime::fetch(svr_frame_rep *out) {
  svr_frame_req req{};
  req.h = {SVR_FRAME, sizeof(req)};
  {
    std::lock_guard<std::mutex> g(m_);
    if (!transport_) return false;
    req.last_frame_id = frame_.frame_id;
  }
  svr_frame_rep rep{};
  int r = transport_->call(&req.h, &rep.h, sizeof(rep));
  int64_t now = now_ns();
  if (r || rep.h.op != SVR_FRAME || rep.h.size != sizeof(rep)) {
    SVR_ERR("frame request failed (%d)", r);
    return false;
  }
  std::lock_guard<std::mutex> g(m_);
  clock_offset_ = now - rep.host_now;
  latest_ = rep;
  hello_.connected = rep.connected;
  // device connection changes become OpenVR events
  bool conn[kDeviceCount] = {rep.connected != 0,
                             rep.connected && (rep.pose[SVR_DEV_LEFT_GRIP].flags & SVR_POSE_VALID),
                             rep.connected && (rep.pose[SVR_DEV_RIGHT_GRIP].flags & SVR_POSE_VALID)};
  for (uint32_t i = 0; i < kDeviceCount; i++) {
    if (conn[i] != connected_[i]) {
      connected_[i] = conn[i];
      vr::VREvent_t ev{};
      ev.eventType = conn[i] ? vr::VREvent_TrackedDeviceActivated : vr::VREvent_TrackedDeviceDeactivated;
      ev.trackedDeviceIndex = i;
      events_.push_back(ev);
    }
  }
  if (out) *out = rep;
  return true;
}

bool Runtime::poll() { return fetch(nullptr); }

bool Runtime::wait_frame() {
  {
    std::lock_guard<std::mutex> g(m_);
    if (pending_valid_) flush_submit_locked();
  }
  svr_frame_rep rep;
  if (!fetch(&rep)) return false;
  int64_t wake = host_to_guest(rep.wake_up);
  if (wake > now_ns()) {
    sleep_until(wake);
    if (!fetch(&rep)) return false;  // fresher prediction for the same frame
  }
  std::lock_guard<std::mutex> g(m_);
  if (frame_.frame_id) last_vsync_ = host_to_guest(frame_.predicted_display);
  frame_ = rep;
  SVR_TRACE("frame %llu display in %.2f ms", (unsigned long long)rep.frame_id,
            (host_to_guest(rep.predicted_display) - now_ns()) / 1e6);
  return true;
}

svr_pose Runtime::frame_pose(int dev) const {
  std::lock_guard<std::mutex> g(m_);
  return frame_.pose[dev];
}

int64_t Runtime::frame_display_guest() const {
  std::lock_guard<std::mutex> g(m_);
  return host_to_guest(frame_.predicted_display);
}

uint64_t Runtime::frame_id() const {
  std::lock_guard<std::mutex> g(m_);
  return frame_.frame_id;
}

svr_pose Runtime::pose_at(int dev, int64_t guest_t) const {
  std::lock_guard<std::mutex> g(m_);
  // the snapshot's poses are predicted to its predicted_display
  float dt = (float)(guest_t - host_to_guest(latest_.predicted_display)) * 1e-9f;
  return extrapolate(latest_.pose[dev], dt);
}

static int device_to_svr(vr::TrackedDeviceIndex_t i) {
  return i == kHmd ? SVR_DEV_HEAD : i == kLeft ? SVR_DEV_LEFT_GRIP : i == kRight ? SVR_DEV_RIGHT_GRIP : -1;
}

bool Runtime::device_connected(vr::TrackedDeviceIndex_t i) const {
  std::lock_guard<std::mutex> g(m_);
  return i < kDeviceCount && connected_[i];
}

vr::TrackedDevicePose_t Runtime::device_pose(vr::TrackedDeviceIndex_t i, float seconds_from_now) const {
  int d = device_to_svr(i);
  if (d < 0) return to_openvr(svr_pose{}, false);
  return to_openvr(pose_at(d, now_ns() + (int64_t)(seconds_from_now * 1e9f)), device_connected(i));
}

vr::TrackedDevicePose_t Runtime::device_frame_pose(vr::TrackedDeviceIndex_t i) const {
  int d = device_to_svr(i);
  if (d < 0) return to_openvr(svr_pose{}, false);
  return to_openvr(frame_pose(d), device_connected(i));
}

int64_t Runtime::last_vsync_guest() const {
  std::lock_guard<std::mutex> g(m_);
  return last_vsync_;
}

float Runtime::input(int hand, int id) const {
  std::lock_guard<std::mutex> g(m_);
  return latest_.input[hand][id];
}

uint32_t Runtime::profile() const {
  std::lock_guard<std::mutex> g(m_);
  return latest_.profile ? latest_.profile : hello_.profile;
}

vr::EVRCompositorError Runtime::submit(vr::EVREye eye, const vr::Texture_t *tex, const vr::VRTextureBounds_t *bounds,
                                       vr::EVRSubmitFlags flags) {
  if (eye != vr::Eye_Left && eye != vr::Eye_Right) return vr::VRCompositorError_IndexOutOfRange;
  if (!tex || !tex->handle) return vr::VRCompositorError_InvalidTexture;
  if (tex->eType != vr::TextureType_Metal) {
    SVR_ERR("unsupported texture type %d (only DXMT textures via TextureType_Metal)", (int)tex->eType);
    return vr::VRCompositorError_TextureUsesUnsupportedFormat;
  }
  auto *mt = (const svr_metal_texture *)tex->handle;
  if (mt->magic != SVR_METAL_TEXTURE_MAGIC || mt->size < sizeof(*mt)) {
    SVR_ERR("TextureType_Metal handle is not a steamac DXMT texture description");
    return vr::VRCompositorError_InvalidTexture;
  }
  std::lock_guard<std::mutex> g(m_);
  if (!transport_) return vr::VRCompositorError_DoNotHaveFocus;
  if (pending_valid_ && (pending_.frame_id != frame_.frame_id || pending_.eye[eye].handle)) flush_submit_locked();
  if (!pending_valid_) {
    memset(&pending_, 0, sizeof(pending_));
    pending_.h = {SVR_SUBMIT, sizeof(pending_)};
    pending_.frame_id = frame_.frame_id;
    pending_.display_time = frame_.predicted_display;
    pending_.head = frame_.pose[SVR_DEV_HEAD];
    pending_valid_ = true;
  }
  svr_eye &e = pending_.eye[eye];
  e.handle = mt->handle;
  e.version = mt->version;
  e.width = mt->width, e.height = mt->height, e.format = mt->format;
  e.array_size = mt->array_size, e.sample_count = mt->sample_count;
  // single-pass stereo: a 2-slice array texture, slice = eye (OpenVR's TextureType_Metal convention)
  e.array_index = mt->array_size > 1 ? (uint32_t)eye : 0;
  if (bounds) {
    e.bounds[0] = bounds->uMin, e.bounds[1] = bounds->vMin, e.bounds[2] = bounds->uMax, e.bounds[3] = bounds->vMax;
  } else {
    e.bounds[0] = 0, e.bounds[1] = 0, e.bounds[2] = 1, e.bounds[3] = 1;
  }
  e.color_space = tex->eColorSpace;
  e.submit_flags = flags;
  if (mt->event_handle) pending_.event_handle = mt->event_handle, pending_.event_value = mt->event_value;
  if (pending_.eye[0].handle && pending_.eye[1].handle) flush_submit_locked();
  return vr::VRCompositorError_None;
}

void Runtime::flush_submit_locked() {
  pending_valid_ = false;
  svr_ack ack{};
  int r = transport_ ? transport_->call(&pending_.h, &ack.h, sizeof(ack)) : -ENOTCONN;
  if (r || ack.status) SVR_ERR("submit of frame %llu failed (%d/%d)", (unsigned long long)pending_.frame_id, r, ack.status);
  else frames_presented_++;
}

void Runtime::post_present_handoff() {
  std::lock_guard<std::mutex> g(m_);
  if (pending_valid_) flush_submit_locked();
}

void Runtime::clear_frame() {
  std::lock_guard<std::mutex> g(m_);
  pending_valid_ = false;
}

void Runtime::haptic(int hand, float duration_s, float frequency, float amplitude) {
  svr_haptic_req req{};
  req.h = {SVR_HAPTIC, sizeof(req)};
  req.hand = (uint32_t)hand;
  req.duration_s = duration_s, req.frequency = frequency, req.amplitude = amplitude;
  svr_ack ack{};
  std::lock_guard<std::mutex> g(m_);
  if (transport_) transport_->call(&req.h, &ack.h, sizeof(ack));
}

void Runtime::push_event(vr::EVREventType type, vr::TrackedDeviceIndex_t dev) {
  std::lock_guard<std::mutex> g(m_);
  vr::VREvent_t ev{};
  ev.eventType = type;
  ev.trackedDeviceIndex = dev;
  events_.push_back(ev);
}

bool Runtime::pop_event(vr::VREvent_t *ev, uint32_t size) {
  std::lock_guard<std::mutex> g(m_);
  if (events_.empty()) return false;
  vr::VREvent_t e = events_.front();
  events_.pop_front();
  if (ev) {
    e.eventAgeSeconds = 0;
    memcpy(ev, &e, size < sizeof(e) ? size : sizeof(e));
  }
  return true;
}
}  // namespace svr
