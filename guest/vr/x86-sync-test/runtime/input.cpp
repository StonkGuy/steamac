// SPDX-License-Identifier: Apache-2.0
// IVRInput: SteamVR Input action manifests, the app's default binding file for the headset's controller type,
// action state from the host's input snapshot, pose actions, haptic output, finger-curl summaries.
#include <math.h>
#include <string.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "input_bindings.h"
#include "interfaces.h"
#include "json.h"
#include "log.h"
#include "runtime.h"
#include "stubs.gen.h"
#include "vrmath.h"

namespace svr {
namespace {
std::string lower(std::string s) {
  for (auto &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

struct ActionState {
  bool digital = false, prev_digital = false, active = false;
  float v[3] = {}, prev_v[3] = {};
  uint64_t origin = 0;
  int64_t changed_at = 0;
};

class Input : public StubIVRInput {
 public:
  EVRInputError SetActionManifestPath(const char *path) override {
    std::lock_guard<std::mutex> g(m_);
    std::string err;
    if (!manifest_.load(path ? path : "", controller_type(), &err)) {
      SVR_ERR("action manifest %s: %s", path ? path : "(null)", err.c_str());
      return VRInputError_InvalidParam;
    }
    SVR_INFO("action manifest %s: %zu actions, %zu bindings for %s", path, manifest_.actions.size(),
             manifest_.bindings.size(), manifest_.binding_controller.c_str());
    for (auto &a : manifest_.actions) handle(a.name);
    return VRInputError_None;
  }

  EVRInputError GetActionSetHandle(const char *name, VRActionSetHandle_t *h) override {
    if (!name || !h) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    *h = handle(name);
    return VRInputError_None;
  }
  EVRInputError GetActionHandle(const char *name, VRActionHandle_t *h) override {
    if (!name || !h) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    *h = handle(name);
    return VRInputError_None;
  }
  EVRInputError GetInputSourceHandle(const char *path, VRInputValueHandle_t *h) override {
    if (!path || !h) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    *h = handle(path);
    return VRInputError_None;
  }

  EVRInputError UpdateActionState(VRActiveActionSet_t *sets, uint32_t set_size, uint32_t count) override {
    if (set_size < offsetof(VRActiveActionSet_t, ulSecondaryActionSet) || (count && !sets))
      return VRInputError_InvalidParam;
    Runtime &rt = Runtime::get();
    rt.poll();
    std::lock_guard<std::mutex> g(m_);
    std::vector<std::string> active;
    for (uint32_t i = 0; i < count; i++) {
      auto *s = (const VRActiveActionSet_t *)((const char *)sets + (size_t)i * set_size);
      if (s->ulActionSet && s->ulActionSet <= paths_.size()) active.push_back(paths_[s->ulActionSet - 1]);
    }
    InputSnapshot snap;
    for (int h = 0; h < 2; h++)
      for (int k = 0; k < SVR_IN_COUNT; k++) snap.v[h][k] = rt.input(h, k);
    for (auto &kv : state_) {
      ActionState &st = kv.second;
      st.prev_digital = st.digital;
      memcpy(st.prev_v, st.v, sizeof(st.v));
      st.digital = false, st.active = false;
      memset(st.v, 0, sizeof(st.v));
      st.origin = 0;
    }
    int64_t now = now_ns();
    evaluate(manifest_, active, snap, now, [&](const std::string &action, int hand, const ActionValue &v) {
      ActionState &st = state_[lower(action)];
      st.active = true;
      bool stronger = v.digital > st.digital || fabsf(v.v[0]) + fabsf(v.v[1]) > fabsf(st.v[0]) + fabsf(st.v[1]);
      if (!st.origin || stronger) st.origin = handle(hand ? "/user/hand/right" : "/user/hand/left");
      st.digital = st.digital || v.digital;
      for (int i = 0; i < 3; i++)
        if (fabsf(v.v[i]) > fabsf(st.v[i])) st.v[i] = v.v[i];
    });
    for (auto &kv : state_) {
      ActionState &st = kv.second;
      if (st.digital != st.prev_digital || memcmp(st.v, st.prev_v, sizeof(st.v))) st.changed_at = now;
    }
    update_time_ = now;
    return VRInputError_None;
  }

  EVRInputError GetDigitalActionData(VRActionHandle_t a, InputDigitalActionData_t *d, uint32_t size,
                                     VRInputValueHandle_t restrict_to) override {
    if (!d || size < sizeof(*d)) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    const Action *act = action(a);
    if (!act) return VRInputError_InvalidHandle;
    if (act->type != "boolean") return VRInputError_WrongType;
    memset(d, 0, sizeof(*d));
    ActionState st = restricted(a, restrict_to);
    d->bActive = st.active;
    d->activeOrigin = st.origin;
    d->bState = st.digital;
    d->bChanged = st.digital != st.prev_digital;
    d->fUpdateTime = -(float)((now_ns() - st.changed_at) * 1e-9);
    return VRInputError_None;
  }

  EVRInputError GetAnalogActionData(VRActionHandle_t a, InputAnalogActionData_t *d, uint32_t size,
                                    VRInputValueHandle_t restrict_to) override {
    if (!d || size < sizeof(*d)) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    const Action *act = action(a);
    if (!act) return VRInputError_InvalidHandle;
    if (act->type != "vector1" && act->type != "vector2" && act->type != "vector3") return VRInputError_WrongType;
    memset(d, 0, sizeof(*d));
    ActionState st = restricted(a, restrict_to);
    d->bActive = st.active;
    d->activeOrigin = st.origin;
    d->x = st.v[0], d->y = st.v[1], d->z = st.v[2];
    d->deltaX = st.v[0] - st.prev_v[0], d->deltaY = st.v[1] - st.prev_v[1], d->deltaZ = st.v[2] - st.prev_v[2];
    d->fUpdateTime = -(float)((now_ns() - st.changed_at) * 1e-9);
    return VRInputError_None;
  }

  EVRInputError GetPoseActionDataRelativeToNow(VRActionHandle_t a, ETrackingUniverseOrigin, float seconds,
                                               InputPoseActionData_t *d, uint32_t size,
                                               VRInputValueHandle_t restrict_to) override {
    return pose_data(a, now_ns() + (int64_t)(seconds * 1e9f), d, size, restrict_to);
  }
  EVRInputError GetPoseActionDataForNextFrame(VRActionHandle_t a, ETrackingUniverseOrigin, InputPoseActionData_t *d,
                                              uint32_t size, VRInputValueHandle_t restrict_to) override {
    return pose_data(a, Runtime::get().frame_display_guest(), d, size, restrict_to);
  }

  EVRInputError GetSkeletalActionData(VRActionHandle_t a, InputSkeletalActionData_t *d, uint32_t size) override {
    if (!d || size < sizeof(*d)) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    int hand = skeleton_hand(a);
    if (hand < -1) return VRInputError_InvalidHandle;
    memset(d, 0, sizeof(*d));
    d->bActive = hand >= 0 && Runtime::get().device_connected(hand ? kRight : kLeft);
    d->activeOrigin = hand >= 0 ? handle(hand ? "/user/hand/right" : "/user/hand/left") : 0;
    return VRInputError_None;
  }

  EVRInputError GetSkeletalTrackingLevel(VRActionHandle_t, EVRSkeletalTrackingLevel *level) override {
    if (level) *level = VRSkeletalTracking_Partial;
    return VRInputError_None;
  }

  EVRInputError GetSkeletalSummaryData(VRActionHandle_t a, EVRSummaryType, VRSkeletalSummaryData_t *s) override {
    if (!s) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    int hand = skeleton_hand(a);
    if (hand < 0) return VRInputError_InvalidHandle;
    Runtime &rt = Runtime::get();
    finger_curls(hand, [&](int id) { return rt.input(hand, id); }, s->flFingerCurl);
    for (float &f : s->flFingerSplay) f = 0.2f;
    return VRInputError_None;
  }

  EVRInputError TriggerHapticVibrationAction(VRActionHandle_t a, float start, float duration, float frequency,
                                             float amplitude, VRInputValueHandle_t restrict_to) override {
    std::vector<int> hands;
    {
      std::lock_guard<std::mutex> g(m_);
      const Action *act = action(a);
      if (!act) return VRInputError_InvalidHandle;
      if (act->type != "vibration") return VRInputError_WrongType;
      std::string want = restrict_to && restrict_to <= paths_.size() ? paths_[restrict_to - 1] : "";
      for (auto &b : manifest_.haptics)
        if (lower(b.output) == lower(act->name)) {
          if (!want.empty() && want != (b.hand ? "/user/hand/right" : "/user/hand/left")) continue;
          hands.push_back(b.hand);
        }
    }
    (void)start;  // the host schedules at receipt; start offsets are below one frame in practice
    for (int h : hands) Runtime::get().haptic(h, duration, frequency, amplitude);
    return VRInputError_None;
  }

  EVRInputError GetActionOrigins(VRActionSetHandle_t, VRActionHandle_t a, VRInputValueHandle_t *out,
                                 uint32_t n) override {
    std::lock_guard<std::mutex> g(m_);
    const Action *act = action(a);
    if (!act) return VRInputError_InvalidHandle;
    std::vector<VRInputValueHandle_t> o;
    for (int h : bound_hands(act->name)) o.push_back(handle(h ? "/user/hand/right" : "/user/hand/left"));
    for (uint32_t i = 0; i < n; i++) out[i] = i < o.size() ? o[i] : k_ulInvalidInputValueHandle;
    return VRInputError_None;
  }

  EVRInputError GetOriginLocalizedName(VRInputValueHandle_t origin, char *buf, uint32_t size, int32_t) override {
    std::lock_guard<std::mutex> g(m_);
    if (!origin || origin > paths_.size() || !buf || !size) return VRInputError_InvalidHandle;
    const std::string &p = paths_[origin - 1];
    std::string name = p == "/user/hand/left" ? "Left Hand" : p == "/user/hand/right" ? "Right Hand" : p;
    snprintf(buf, size, "%s", name.c_str());
    return VRInputError_None;
  }

  EVRInputError GetOriginTrackedDeviceInfo(VRInputValueHandle_t origin, InputOriginInfo_t *info, uint32_t size) override {
    if (!info || size < sizeof(*info)) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    if (!origin || origin > paths_.size()) return VRInputError_InvalidHandle;
    const std::string &p = paths_[origin - 1];
    memset(info, 0, sizeof(*info));
    info->devicePath = origin;
    info->trackedDeviceIndex = p == "/user/hand/left" ? kLeft : p == "/user/hand/right" ? kRight
                               : p == "/user/head" ? kHmd : k_unTrackedDeviceIndexInvalid;
    return VRInputError_None;
  }

  EVRInputError GetActionBindingInfo(VRActionHandle_t a, InputBindingInfo_t *info, uint32_t size, uint32_t count,
                                     uint32_t *returned) override {
    std::lock_guard<std::mutex> g(m_);
    const Action *act = action(a);
    if (!act) return VRInputError_InvalidHandle;
    uint32_t n = 0;
    for (auto &b : manifest_.bindings) {
      if (lower(b.output) != lower(act->name)) continue;
      if (info && n < count && size >= sizeof(InputBindingInfo_t)) {
        InputBindingInfo_t &bi = *(InputBindingInfo_t *)((char *)info + (size_t)n * size);
        memset(&bi, 0, sizeof(bi));
        snprintf(bi.rchDevicePathName, sizeof(bi.rchDevicePathName), "%s", b.hand ? "/user/hand/right" : "/user/hand/left");
        snprintf(bi.rchInputPathName, sizeof(bi.rchInputPathName), "%s", b.input_path.c_str());
        snprintf(bi.rchModeName, sizeof(bi.rchModeName), "%s", b.mode.c_str());
        snprintf(bi.rchSlotName, sizeof(bi.rchSlotName), "%s", b.slot.c_str());
        snprintf(bi.rchInputSourceType, sizeof(bi.rchInputSourceType), "%s", "controller");
      }
      n++;
    }
    if (returned) *returned = std::min(n, count);
    return info && n > count ? VRInputError_BufferTooSmall : VRInputError_None;
  }

  EVRInputError GetDominantHand(ETrackedControllerRole *r) override {
    if (r) *r = TrackedControllerRole_RightHand;
    return VRInputError_None;
  }
  EVRInputError SetDominantHand(ETrackedControllerRole) override { return VRInputError_None; }
  EVRInputError ShowActionOrigins(VRActionSetHandle_t, VRActionHandle_t) override { return VRInputError_None; }
  EVRInputError ShowBindingsForActionSet(VRActiveActionSet_t *, uint32_t, uint32_t, VRInputValueHandle_t) override {
    return VRInputError_None;
  }
  bool IsUsingLegacyInput() override { return false; }
  EVRInputError OpenBindingUI(const char *, VRActionSetHandle_t, VRInputValueHandle_t, bool) override {
    return VRInputError_None;
  }
  EVRInputError GetBindingVariant(vr::VRInputValueHandle_t, char *buf, uint32_t size) override {
    if (buf && size) buf[0] = 0;
    return VRInputError_None;
  }

  void reset() {
    std::lock_guard<std::mutex> g(m_);
    manifest_ = Manifest{};
    state_.clear();
    paths_.clear();
    handles_.clear();
  }

 private:
  static std::string controller_type() { return controller_info(0).controller_type; }

  // path handles are 1-based indices into paths_ (OpenVR handles are case-insensitive)
  uint64_t handle(const std::string &path) {
    std::string p = lower(path);
    auto it = handles_.find(p);
    if (it != handles_.end()) return it->second;
    paths_.push_back(p);
    return handles_[p] = paths_.size();
  }
  const Action *action(VRActionHandle_t h) const {
    if (!h || h > paths_.size()) return nullptr;
    for (auto &a : manifest_.actions)
      if (lower(a.name) == paths_[h - 1]) return &a;
    return nullptr;
  }
  ActionState restricted(VRActionHandle_t a, VRInputValueHandle_t restrict_to) {
    ActionState st = state_[paths_[a - 1]];
    if (restrict_to && restrict_to != k_ulInvalidInputValueHandle && st.origin && st.origin != restrict_to) {
      // the strongest source was the other hand: recompute is not needed for the common case of
      // one-hand-per-action bindings; report inactive for the restricted device
      st = ActionState{};
    }
    return st;
  }
  std::vector<int> bound_hands(const std::string &action) const {
    std::vector<int> hands;
    auto add = [&](int h) {
      if (std::find(hands.begin(), hands.end(), h) == hands.end()) hands.push_back(h);
    };
    for (auto &b : manifest_.bindings)
      if (lower(b.output) == lower(action)) add(b.hand);
    for (auto &p : manifest_.poses)
      if (lower(p.output) == lower(action)) add(p.hand);
    for (auto &p : manifest_.haptics)
      if (lower(p.output) == lower(action)) add(p.hand);
    return hands;
  }
  // -2 invalid handle, -1 not bound, else hand
  int skeleton_hand(VRActionHandle_t a) const {
    const Action *act = action(a);
    if (!act) return -2;
    if (act->skeleton == "/skeleton/hand/left") return 0;
    if (act->skeleton == "/skeleton/hand/right") return 1;
    for (auto &s : manifest_.skeletons)
      if (lower(s.output) == lower(act->name)) return s.hand;
    return -1;
  }
  EVRInputError pose_data(VRActionHandle_t a, int64_t t, InputPoseActionData_t *d, uint32_t size,
                          VRInputValueHandle_t restrict_to) {
    if (!d || size < sizeof(*d)) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    const Action *act = action(a);
    if (!act) return VRInputError_InvalidHandle;
    if (act->type != "pose") return VRInputError_WrongType;
    memset(d, 0, sizeof(*d));
    std::string want = restrict_to && restrict_to <= paths_.size() ? paths_[restrict_to - 1] : "";
    for (auto &p : manifest_.poses) {
      if (lower(p.output) != lower(act->name)) continue;
      std::string dev = p.hand ? "/user/hand/right" : "/user/hand/left";
      if (!want.empty() && want != dev) continue;
      Runtime &rt = Runtime::get();
      bool conn = rt.device_connected(p.hand ? kRight : kLeft);
      int src = p.aim ? (p.hand ? SVR_DEV_RIGHT_AIM : SVR_DEV_LEFT_AIM) : (p.hand ? SVR_DEV_RIGHT_GRIP : SVR_DEV_LEFT_GRIP);
      d->bActive = conn;
      d->activeOrigin = handle(dev);
      d->pose = to_openvr(rt.pose_at(src, t), conn);
      return VRInputError_None;
    }
    return VRInputError_None;  // unbound: inactive, identity-free zero pose
  }

  std::mutex m_;
  Manifest manifest_;
  std::map<std::string, ActionState> state_;
  std::vector<std::string> paths_;
  std::map<std::string, uint64_t> handles_;
  int64_t update_time_ = 0;
};

Input &instance() {
  static Input i;
  return i;
}
}  // namespace

void *input_iface() { return static_cast<vr::IVRInput *>(&instance()); }
void input_reset() { instance().reset(); }
}  // namespace svr
