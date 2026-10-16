// SPDX-License-Identifier: Apache-2.0
// IVRInput: SteamVR Input action manifests, the app's default binding file for the headset's controller type,
// action state from the host's input snapshot, pose actions, haptic output, finger-curl summaries.
//
// Handles: every path an application names gets a handle, whether or not a manifest is loaded and whether or
// not the manifest lists it (applications fetch handles before the manifest is known, and the manifest may
// arrive in VR_Init's startup info instead of SetActionManifestPath). Reading an action the manifest does not
// have is not an error: it is inactive. Only k_ulInvalidActionHandle and values this runtime never handed out
// are VRInputError_InvalidHandle. Engines log every failed Get*ActionData call, hundreds per frame.
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
#include "skeleton.h"
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

// what a handle names: its path, the manifest action of that name if any, and that action's state for
// any device / the left hand / the right hand (ulRestrictToDevice)
struct Slot {
  std::string path;
  int action = -1;
  ActionState st[3];
};
const char *const kHandPath[2] = {"/user/hand/left", "/user/hand/right"};

class Input : public StubIVRInput {
 public:
  Input() {
    for (int h = 0; h < 2; h++) hand_handle_[h] = handle(kHandPath[h]);
  }

  EVRInputError SetActionManifestPath(const char *path) override {
    std::lock_guard<std::mutex> g(m_);
    if (path && !manifest_path_.empty() && manifest_path_ == path) return VRInputError_None;  // e.g. after startup info
    Manifest m;
    std::string err;
    if (!m.load(path ? path : "", controller_type(), &err)) {
      SVR_ERR("action manifest %s: %s", path ? path : "(null)", err.c_str());
      return VRInputError_InvalidParam;
    }
    if (!err.empty()) SVR_ERR("action manifest %s: %s (actions stay unbound)", path, err.c_str());
    for (Slot &s : slots_) s.action = -1, s.st[0] = s.st[1] = s.st[2] = ActionState{};
    manifest_ = std::move(m);
    manifest_path_ = path;
    action_slots_.clear();
    for (size_t i = 0; i < manifest_.actions.size(); i++) {
      uint64_t h = handle(manifest_.actions[i].name);
      slots_[h - 1].action = (int)i;
      action_slots_.push_back(h - 1);
    }
    SVR_INFO("action manifest %s: %zu actions, %zu bindings for %s", path, manifest_.actions.size(),
             manifest_.bindings.size(), manifest_.binding_controller.c_str());
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
    std::vector<ActiveSet> active;
    bool with_secondary = set_size >= offsetof(VRActiveActionSet_t, ulSecondaryActionSet) + sizeof(VRActionSetHandle_t);
    for (uint32_t i = 0; i < count; i++) {
      auto *s = (const VRActiveActionSet_t *)((const char *)sets + (size_t)i * set_size);
      // restricted to a device that is not a hand: no controller binding of the set applies
      int src = source(s->ulRestrictedToDevice);
      unsigned hands = src == 0 ? 3u : src > 0 ? 1u << (src - 1) : 0u;
      if (s->ulActionSet && s->ulActionSet <= slots_.size() && hands) active.push_back({slots_[s->ulActionSet - 1].path, hands});
      if (with_secondary && src > 0 && s->ulSecondaryActionSet && s->ulSecondaryActionSet <= slots_.size())
        active.push_back({slots_[s->ulSecondaryActionSet - 1].path, 3u & ~hands});
    }
    InputSnapshot snap;
    for (int h = 0; h < 2; h++)
      for (int k = 0; k < SVR_IN_COUNT; k++) snap.v[h][k] = rt.input(h, k);
    for (size_t i : action_slots_)
      for (ActionState &st : slots_[i].st) {
        st.prev_digital = st.digital;
        memcpy(st.prev_v, st.v, sizeof(st.v));
        st.digital = false, st.active = false;
        memset(st.v, 0, sizeof(st.v));
        st.origin = 0;
      }
    int64_t now = now_ns();
    evaluate(manifest_, active, snap, now, [&](const std::string &action, int hand, const ActionValue &v) {
      auto it = handles_.find(action);  // binding outputs are lower-case, like the handle table
      if (it == handles_.end() || slots_[it->second - 1].action < 0) return;  // bound to an action the manifest lacks
      for (ActionState *st : {&slots_[it->second - 1].st[0], &slots_[it->second - 1].st[1 + hand]}) {
        st->active = true;
        bool stronger = v.digital > st->digital || fabsf(v.v[0]) + fabsf(v.v[1]) > fabsf(st->v[0]) + fabsf(st->v[1]);
        if (!st->origin || stronger) st->origin = hand_handle_[hand];
        st->digital = st->digital || v.digital;
        for (int i = 0; i < 3; i++)
          if (fabsf(v.v[i]) > fabsf(st->v[i])) st->v[i] = v.v[i];
      }
    });
    // headset bindings (VRChat's HeadsetOnHead = /user/head/proximity): a connected headset is worn
    for (auto &hb : manifest_.head) {
      if (hb.component != "proximity") continue;
      bool set_active = false;
      for (auto &a : active) set_active |= a.name == hb.set;
      auto it = handles_.find(hb.output);
      if (!set_active || it == handles_.end() || slots_[it->second - 1].action < 0) continue;
      ActionState &st = slots_[it->second - 1].st[0];
      st.active = true;
      st.digital = st.digital || rt.device_connected(kHmd);
      if (st.digital) st.v[0] = 1;
    }
    for (size_t i : action_slots_)
      for (ActionState &st : slots_[i].st)
        if (st.digital != st.prev_digital || memcmp(st.v, st.prev_v, sizeof(st.v))) st.changed_at = now;
    update_time_ = now;
    return VRInputError_None;
  }

  EVRInputError GetDigitalActionData(VRActionHandle_t a, InputDigitalActionData_t *d, uint32_t size,
                                     VRInputValueHandle_t restrict_to) override {
    if (!d || size < sizeof(*d)) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    if (!valid(a)) return VRInputError_InvalidHandle;
    memset(d, 0, sizeof(*d));
    const Action *act = action(a);
    if (!act) return VRInputError_None;  // not in the manifest: inactive
    if (act->type != "boolean") return VRInputError_WrongType;
    const ActionState *st = restricted(a, restrict_to);
    if (!st || !st->active) return VRInputError_None;
    d->bActive = true;
    d->activeOrigin = st->origin;
    d->bState = st->digital;
    d->bChanged = st->digital != st->prev_digital;
    d->fUpdateTime = (float)((st->changed_at - update_time_) * 1e-9);  // relative to now, negative = past
    return VRInputError_None;
  }

  EVRInputError GetAnalogActionData(VRActionHandle_t a, InputAnalogActionData_t *d, uint32_t size,
                                    VRInputValueHandle_t restrict_to) override {
    if (!d || size < sizeof(*d)) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    if (!valid(a)) return VRInputError_InvalidHandle;
    memset(d, 0, sizeof(*d));
    const Action *act = action(a);
    if (!act) return VRInputError_None;  // not in the manifest: inactive
    if (act->type != "vector1" && act->type != "vector2" && act->type != "vector3") return VRInputError_WrongType;
    const ActionState *st = restricted(a, restrict_to);
    if (!st || !st->active) return VRInputError_None;
    d->bActive = true;
    d->activeOrigin = st->origin;
    d->x = st->v[0], d->y = st->v[1], d->z = st->v[2];
    d->deltaX = st->v[0] - st->prev_v[0], d->deltaY = st->v[1] - st->prev_v[1], d->deltaZ = st->v[2] - st->prev_v[2];
    d->fUpdateTime = (float)((st->changed_at - update_time_) * 1e-9);
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
    d->activeOrigin = hand >= 0 ? hand_handle_[hand] : 0;
    return VRInputError_None;
  }

  EVRInputError GetBoneCount(VRActionHandle_t a, uint32_t *count) override {
    if (!count) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    if (skeleton_hand(a) < -1) return VRInputError_InvalidHandle;
    *count = kHandBoneCount;
    return VRInputError_None;
  }
  EVRInputError GetBoneHierarchy(VRActionHandle_t a, BoneIndex_t *parents, uint32_t count) override {
    if (!parents || count < (uint32_t)kHandBoneCount) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    if (skeleton_hand(a) < -1) return VRInputError_InvalidHandle;
    for (int i = 0; i < kHandBoneCount; i++) parents[i] = hand_bone_parent(i);
    return VRInputError_None;
  }
  EVRInputError GetBoneName(VRActionHandle_t a, BoneIndex_t bone, char *name, uint32_t len) override {
    std::lock_guard<std::mutex> g(m_);
    int hand = skeleton_hand(a);
    if (hand < -1) return VRInputError_InvalidHandle;
    const char *n = hand_bone_name(hand > 0, bone);
    if (!n) return VRInputError_InvalidBoneIndex;
    if (!name || len <= strlen(n)) return VRInputError_BufferTooSmall;
    memcpy(name, n, strlen(n) + 1);
    return VRInputError_None;
  }
  EVRInputError GetSkeletalReferenceTransforms(VRActionHandle_t a, EVRSkeletalTransformSpace space,
                                               EVRSkeletalReferencePose pose, VRBoneTransform_t *out,
                                               uint32_t count) override {
    if (!out || count < (uint32_t)kHandBoneCount) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    int hand = skeleton_hand(a);
    if (hand < -1) return VRInputError_InvalidHandle;
    hand_reference_pose(hand > 0, pose, out);
    if (space == VRSkeletalTransformSpace_Model) hand_to_model_space(out, out);
    return VRInputError_None;
  }
  EVRInputError GetSkeletalTrackingLevel(VRActionHandle_t a, EVRSkeletalTrackingLevel *level) override {
    if (!level) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    if (skeleton_hand(a) < -1) return VRInputError_InvalidHandle;
    *level = VRSkeletalTracking_Estimated;  // fingers follow buttons, triggers and touch sensors
    return VRInputError_None;
  }
  EVRInputError GetSkeletalBoneData(VRActionHandle_t a, EVRSkeletalTransformSpace space, EVRSkeletalMotionRange range,
                                    VRBoneTransform_t *out, uint32_t count) override {
    if (!out || count < (uint32_t)kHandBoneCount) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    int hand = skeleton_hand(a);
    if (hand < -1) return VRInputError_InvalidHandle;
    if (hand < 0) return VRInputError_NoData;
    bones(hand, range, out);
    if (space == VRSkeletalTransformSpace_Model) hand_to_model_space(out, out);
    return VRInputError_None;
  }
  EVRInputError GetSkeletalSummaryData(VRActionHandle_t a, EVRSummaryType, VRSkeletalSummaryData_t *s) override {
    if (!s) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    int hand = skeleton_hand(a);
    if (hand < -1) return VRInputError_InvalidHandle;
    if (hand < 0) return VRInputError_NoData;
    Runtime &rt = Runtime::get();
    finger_curls(hand, [&](int id) { return rt.input(hand, id); }, s->flFingerCurl);
    for (float &f : s->flFingerSplay) f = 0.2f;
    return VRInputError_None;
  }
  // "Compressed" bone data is only ever handed back to DecompressSkeletalBoneData of the same runtime: a tag and
  // the parent-space transforms.
  EVRInputError GetSkeletalBoneDataCompressed(VRActionHandle_t a, EVRSkeletalMotionRange range, void *out, uint32_t size,
                                              uint32_t *required) override {
    if (required) *required = sizeof(PackedBones);
    if (!out || size < sizeof(PackedBones)) return VRInputError_BufferTooSmall;
    PackedBones p;
    p.tag = kPackedTag;
    EVRInputError e = GetSkeletalBoneData(a, VRSkeletalTransformSpace_Parent, range, p.bones, kHandBoneCount);
    if (e != VRInputError_None) return e;
    memcpy(out, &p, sizeof(p));
    return VRInputError_None;
  }
  EVRInputError DecompressSkeletalBoneData(const void *in, uint32_t size, EVRSkeletalTransformSpace space,
                                           VRBoneTransform_t *out, uint32_t count) override {
    PackedBones p;
    if (!in || !out || size < sizeof(p) || count < (uint32_t)kHandBoneCount) return VRInputError_InvalidParam;
    memcpy(&p, in, sizeof(p));
    if (p.tag != kPackedTag) return VRInputError_InvalidCompressedData;
    if (space == VRSkeletalTransformSpace_Model) hand_to_model_space(p.bones, p.bones);
    memcpy(out, p.bones, sizeof(p.bones));
    return VRInputError_None;
  }

  EVRInputError TriggerHapticVibrationAction(VRActionHandle_t a, float start, float duration, float frequency,
                                             float amplitude, VRInputValueHandle_t restrict_to) override {
    std::vector<int> hands;
    {
      std::lock_guard<std::mutex> g(m_);
      if (!valid(a)) return VRInputError_InvalidHandle;
      const Action *act = action(a);
      if (!act) return VRInputError_None;
      if (act->type != "vibration") return VRInputError_WrongType;
      int src = source(restrict_to);
      for (auto &b : manifest_.haptics)
        if (b.output == slots_[a - 1].path && (src == 0 || src == 1 + b.hand)) hands.push_back(b.hand);
    }
    (void)start;  // the host schedules at receipt; start offsets are below one frame in practice
    for (int h : hands) Runtime::get().haptic(h, duration, frequency, amplitude);
    return VRInputError_None;
  }

  EVRInputError GetActionOrigins(VRActionSetHandle_t, VRActionHandle_t a, VRInputValueHandle_t *out,
                                 uint32_t n) override {
    std::lock_guard<std::mutex> g(m_);
    if (!valid(a)) return VRInputError_InvalidHandle;
    if (!out && n) return VRInputError_InvalidParam;
    const Action *act = action(a);
    std::vector<VRInputValueHandle_t> o;
    if (act)
      for (int h : bound_hands(act->name)) o.push_back(hand_handle_[h]);
    for (uint32_t i = 0; i < n; i++) out[i] = i < o.size() ? o[i] : k_ulInvalidInputValueHandle;
    return VRInputError_None;
  }

  EVRInputError GetOriginLocalizedName(VRInputValueHandle_t origin, char *buf, uint32_t size, int32_t) override {
    std::lock_guard<std::mutex> g(m_);
    if (!valid(origin) || !buf || !size) return VRInputError_InvalidHandle;
    const std::string &p = slots_[origin - 1].path;
    std::string name = p == "/user/hand/left" ? "Left Hand" : p == "/user/hand/right" ? "Right Hand" : p;
    snprintf(buf, size, "%s", name.c_str());
    return VRInputError_None;
  }

  EVRInputError GetOriginTrackedDeviceInfo(VRInputValueHandle_t origin, InputOriginInfo_t *info, uint32_t size) override {
    if (!info || size < sizeof(*info)) return VRInputError_InvalidParam;
    std::lock_guard<std::mutex> g(m_);
    if (!valid(origin)) return VRInputError_InvalidHandle;
    const std::string &p = slots_[origin - 1].path;
    memset(info, 0, sizeof(*info));
    info->devicePath = origin;
    info->trackedDeviceIndex = p == "/user/hand/left" ? kLeft : p == "/user/hand/right" ? kRight
                               : p == "/user/head" ? kHmd : k_unTrackedDeviceIndexInvalid;
    return VRInputError_None;
  }

  EVRInputError GetActionBindingInfo(VRActionHandle_t a, InputBindingInfo_t *info, uint32_t size, uint32_t count,
                                     uint32_t *returned) override {
    std::lock_guard<std::mutex> g(m_);
    if (!valid(a)) return VRInputError_InvalidHandle;
    uint32_t n = 0;
    for (auto &b : manifest_.bindings) {
      if (b.output != slots_[a - 1].path) continue;
      if (info && n < count && size) {
        InputBindingInfo_t bi;  // older versions of the struct are a prefix of it: the caller's size decides
        memset(&bi, 0, sizeof(bi));
        snprintf(bi.rchDevicePathName, sizeof(bi.rchDevicePathName), "%s", b.hand ? "/user/hand/right" : "/user/hand/left");
        snprintf(bi.rchInputPathName, sizeof(bi.rchInputPathName), "%s", b.input_path.c_str());
        snprintf(bi.rchModeName, sizeof(bi.rchModeName), "%s", b.mode.c_str());
        snprintf(bi.rchSlotName, sizeof(bi.rchSlotName), "%s", b.slot.c_str());
        snprintf(bi.rchInputSourceType, sizeof(bi.rchInputSourceType), "%s", "controller");
        memcpy((char *)info + (size_t)n * size, &bi, std::min((size_t)size, sizeof(bi)));
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

  // handles stay valid for the life of the process: an application may keep them across VR_Shutdown/VR_Init
  void reset() {
    std::lock_guard<std::mutex> g(m_);
    manifest_ = Manifest{};
    manifest_path_.clear();
    action_slots_.clear();
    for (Slot &s : slots_) s.action = -1, s.st[0] = s.st[1] = s.st[2] = ActionState{};
  }

 private:
  static std::string controller_type() { return controller_info(0).controller_type; }

  // path handles are 1-based indices into slots_ (OpenVR paths are case-insensitive)
  uint64_t handle(const std::string &path) {
    std::string p = lower(path);
    auto it = handles_.find(p);
    if (it != handles_.end()) return it->second;
    slots_.push_back(Slot{p, -1, {}});
    return handles_[p] = slots_.size();
  }
  bool valid(uint64_t h) const { return h && h <= slots_.size(); }
  const Action *action(VRActionHandle_t h) const {
    return valid(h) && slots_[h - 1].action >= 0 ? &manifest_.actions[slots_[h - 1].action] : nullptr;
  }
  // 0 any device, 1 left hand, 2 right hand, -1 another device (nothing of ours is bound there)
  int source(VRInputValueHandle_t h) const {
    return h == k_ulInvalidInputValueHandle ? 0 : h == hand_handle_[0] ? 1 : h == hand_handle_[1] ? 2 : -1;
  }
  const ActionState *restricted(VRActionHandle_t a, VRInputValueHandle_t restrict_to) const {
    int src = source(restrict_to);
    return src < 0 ? nullptr : &slots_[a - 1].st[src];
  }
  std::vector<int> bound_hands(const std::string &action) const {
    std::vector<int> hands;
    auto add = [&](int h) {
      if (std::find(hands.begin(), hands.end(), h) == hands.end()) hands.push_back(h);
    };
    std::string name = lower(action);
    for (auto &b : manifest_.bindings)
      if (b.output == name) add(b.hand);
    for (auto &p : manifest_.poses)
      if (p.output == name) add(p.hand);
    for (auto &p : manifest_.haptics)
      if (p.output == name) add(p.hand);
    return hands;
  }
  static const uint32_t kPackedTag = 0x314b5353;  // "SSK1"
  struct PackedBones {
    uint32_t tag;
    VRBoneTransform_t bones[kHandBoneCount];
  };
  void bones(int hand, EVRSkeletalMotionRange range, VRBoneTransform_t *out) {
    Runtime &rt = Runtime::get();
    float raw[5], curl[5];
    const float splay[5] = {0, 0, 0, 0, 0};
    finger_curls(hand, [&](int id) { return rt.input(hand, id); }, raw);
    hand_limit_curls(range, raw, curl);
    hand_pose(hand, curl, splay, out);
  }
  // -2 invalid handle, -1 not bound (or not a manifest action), else hand
  int skeleton_hand(VRActionHandle_t a) const {
    if (!valid(a)) return -2;
    const Action *act = action(a);
    if (!act) return -1;
    if (act->skeleton == "/skeleton/hand/left") return 0;
    if (act->skeleton == "/skeleton/hand/right") return 1;
    for (auto &s : manifest_.skeletons)
      if (s.output == slots_[a - 1].path) return s.hand;
    return -1;
  }
  EVRInputError pose_data(VRActionHandle_t a, int64_t t, InputPoseActionData_t *d, uint32_t size,
                          VRInputValueHandle_t restrict_to) {
    if (!d || size < sizeof(*d)) return VRInputError_InvalidParam;
    Runtime::get().pose_stats().pose_action++;
    EVRInputError r = pose_data_locked(a, t, d, restrict_to);
    if (r == VRInputError_None && !d->bActive) Runtime::get().pose_stats().pose_action_inactive++;
    return r;
  }
  EVRInputError pose_data_locked(VRActionHandle_t a, int64_t t, InputPoseActionData_t *d, VRInputValueHandle_t restrict_to) {
    std::lock_guard<std::mutex> g(m_);
    if (!valid(a)) return VRInputError_InvalidHandle;
    memset(d, 0, sizeof(*d));
    const Action *act = action(a);
    if (!act) return VRInputError_None;
    int want = source(restrict_to);
    if (act->type == "skeleton") {
      // SteamVR answers a pose read of a skeleton action with the pose of the hand it animates (the Unity
      // plugin reads its skeleton actions this way every frame)
      int hand = skeleton_hand(a);
      if (hand < 0 || (want != 0 && want != 1 + hand)) return VRInputError_None;
      Runtime &rt = Runtime::get();
      bool conn = rt.device_connected(hand ? kRight : kLeft);
      d->bActive = conn;
      d->activeOrigin = hand_handle_[hand];
      d->pose = to_openvr(rt.pose_at(hand ? SVR_DEV_RIGHT_GRIP : SVR_DEV_LEFT_GRIP, t), conn);
      return VRInputError_None;
    }
    if (act->type != "pose") return VRInputError_WrongType;
    for (auto &p : manifest_.poses) {
      if (p.output != slots_[a - 1].path) continue;
      if (want != 0 && want != 1 + p.hand) continue;
      Runtime &rt = Runtime::get();
      bool conn = rt.device_connected(p.hand ? kRight : kLeft);
      int src = p.aim ? (p.hand ? SVR_DEV_RIGHT_AIM : SVR_DEV_LEFT_AIM) : (p.hand ? SVR_DEV_RIGHT_GRIP : SVR_DEV_LEFT_GRIP);
      d->bActive = conn;
      d->activeOrigin = hand_handle_[p.hand];
      d->pose = to_openvr(rt.pose_at(src, t), conn);
      return VRInputError_None;
    }
    return VRInputError_None;  // unbound: inactive, identity-free zero pose
  }

  std::mutex m_;
  Manifest manifest_;
  std::string manifest_path_;
  std::vector<Slot> slots_;           // handle - 1 → path, action, state
  std::vector<size_t> action_slots_;  // the slots of the manifest's actions
  std::map<std::string, uint64_t> handles_;
  uint64_t hand_handle_[2] = {};
  int64_t update_time_ = 0;
};

Input &instance() {
  static Input i;
  return i;
}
}  // namespace

void *input_iface() { return static_cast<vr::IVRInput *>(&instance()); }
void input_reset() { instance().reset(); }

// VR_Init's startup info: {"action_manifest_path": "..."} is how Unity's OpenVR XR plugin names the manifest
// (the SteamVR Unity plugin then never calls SetActionManifestPath)
void input_startup_info(const char *startup) {
  Json root;
  std::string err;
  if (!startup || !*startup || !Json::parse(startup, &root, &err) || !root.is_obj()) return;
  std::string path = root.get_str("action_manifest_path");
  if (!path.empty()) instance().SetActionManifestPath(path.c_str());
}
}  // namespace svr
