// SPDX-License-Identifier: Apache-2.0
// IVROverlay: overlays are created, configured and queried like under SteamVR, so a game that sets up a
// dashboard or in-world overlay at start-up keeps running. The state is tracked here only: overlays are not
// composited into the frame yet and there is no dashboard, so no overlay ever gets input events or focus
// (docs/vr-guest.md, "Overlays").
#include <string.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <string>

#include "interfaces.h"
#include "stubs.gen.h"
#include "vrmath.h"

namespace svr {
namespace {
const uint32_t kMaxOverlays = 128;  // SteamVR's k_unMaxOverlayCount

struct OverlayState {
  std::string key, name;
  bool dashboard = false, thumbnail = false, visible = false, has_texture = false;
  uint32_t flags = 0, sort_order = 0, pid = 0, scene_pid = 0, tex_w = 0, tex_h = 0;
  float color[3] = {1, 1, 1}, alpha = 1, texel_aspect = 1, width = 1, curvature = 0, pre_curve_pitch = 0;
  EColorSpace color_space = ColorSpace_Auto;
  VRTextureBounds_t bounds{0, 0, 1, 1};
  VROverlayTransformType transform_type = VROverlayTransform_Absolute;
  ETrackingUniverseOrigin origin = TrackingUniverseStanding;
  HmdMatrix34_t transform = identity34();
  TrackedDeviceIndex_t device = k_unTrackedDeviceIndexInvalid;
  std::string component;
  HmdVector2_t cursor{{0, 0}}, mouse_scale{{1, 1}};
  VROverlayInputMethod input = VROverlayInputMethod_None;
};

const char *error_name(EVROverlayError e) {
  switch (e) {
#define N(x) \
  case VROverlayError_##x: \
    return "VROverlayError_" #x;
    N(None) N(UnknownOverlay) N(InvalidHandle) N(PermissionDenied) N(OverlayLimitExceeded) N(WrongVisibilityType)
    N(KeyTooLong) N(NameTooLong) N(KeyInUse) N(WrongTransformType) N(InvalidTrackedDevice) N(InvalidParameter)
    N(ThumbnailCantBeDestroyed) N(ArrayTooSmall) N(RequestFailed) N(InvalidTexture) N(UnableToLoadFile)
    N(KeyboardAlreadyInUse) N(NoNeighbor) N(TooManyMaskPrimitives) N(BadMaskPrimitive) N(TextureAlreadyLocked)
    N(TextureLockCapacityReached) N(TextureNotLocked) N(TimedOut)
#undef N
  }
  return "VROverlayError_Unknown";
}

class Overlay : public StubIVROverlay {
  typedef VROverlayHandle_t H;
  typedef EVROverlayError E;

 public:
  E FindOverlay(const char *key, H *h) override {
    std::lock_guard<std::mutex> g(m_);
    if (h) *h = k_ulOverlayHandleInvalid;
    if (!key || !h) return VROverlayError_InvalidParameter;
    for (auto &o : overlays_)
      if (o.second.key == key) return *h = o.first, VROverlayError_None;
    return VROverlayError_UnknownOverlay;
  }
  E CreateOverlay(const char *key, const char *name, H *h) override {
    std::lock_guard<std::mutex> g(m_);
    return create(key, name, h);
  }
  E CreateSubviewOverlay(H parent, const char *key, const char *name, H *h) override {
    std::lock_guard<std::mutex> g(m_);
    if (h) *h = k_ulOverlayHandleInvalid;
    if (!find(parent)) return VROverlayError_InvalidHandle;
    E e = create(key, name, h);
    if (e == VROverlayError_None) overlays_[*h].transform_type = VROverlayTransform_Subview;
    return e;
  }
  E CreateDashboardOverlay(const char *key, const char *name, H *h, H *thumb) override {
    std::lock_guard<std::mutex> g(m_);
    if (thumb) *thumb = k_ulOverlayHandleInvalid;
    E e = create(key, name, h);
    if (e != VROverlayError_None) return e;
    OverlayState &o = overlays_[*h];
    o.dashboard = true;
    o.transform_type = VROverlayTransform_DashboardTab;
    o.input = VROverlayInputMethod_Mouse;
    H t = k_ulOverlayHandleInvalid;
    e = create((std::string("thumbnail.") + key).c_str(), name, &t);
    if (e != VROverlayError_None) {
      overlays_.erase(*h);
      *h = k_ulOverlayHandleInvalid;
      return e;
    }
    overlays_[t].thumbnail = true;
    overlays_[t].transform_type = VROverlayTransform_DashboardThumb;
    thumbs_[*h] = t;
    if (thumb) *thumb = t;
    return VROverlayError_None;
  }
  E DestroyOverlay(H h) override {
    std::lock_guard<std::mutex> g(m_);
    OverlayState *o = find(h);
    if (!o) return VROverlayError_InvalidHandle;
    if (o->thumbnail) return VROverlayError_ThumbnailCantBeDestroyed;
    auto t = thumbs_.find(h);
    if (t != thumbs_.end()) {
      overlays_.erase(t->second);
      thumbs_.erase(t);
    }
    overlays_.erase(h);
    return VROverlayError_None;
  }
  uint32_t GetOverlayKey(H h, char *buf, uint32_t len, E *e) override { return str(h, buf, len, e, false); }
  uint32_t GetOverlayName(H h, char *buf, uint32_t len, E *e) override { return str(h, buf, len, e, true); }
  E SetOverlayName(H h, const char *name) override {
    if (!name) return VROverlayError_InvalidParameter;
    if (strlen(name) >= k_unVROverlayMaxNameLength) return VROverlayError_NameTooLong;
    return with(h, [&](OverlayState &o) { o.name = name; });
  }
  E GetOverlayImageData(H h, void *, uint32_t, uint32_t *w, uint32_t *ht) override {
    return with(h, [&](OverlayState &o) {
      if (w) *w = o.tex_w;
      if (ht) *ht = o.tex_h;
    }, VROverlayError_RequestFailed);  // pixels are not kept
  }
  const char *GetOverlayErrorNameFromEnum(E e) override { return error_name(e); }
  E SetOverlayRenderingPid(H h, uint32_t pid) override { return with(h, [&](OverlayState &o) { o.pid = pid; }); }
  uint32_t GetOverlayRenderingPid(H h) override {
    uint32_t pid = 0;
    with(h, [&](OverlayState &o) { pid = o.pid; });
    return pid;
  }
  E SetOverlayFlag(H h, VROverlayFlags f, bool on) override {
    return with(h, [&](OverlayState &o) { o.flags = on ? o.flags | (uint32_t)f : o.flags & ~(uint32_t)f; });
  }
  E GetOverlayFlag(H h, VROverlayFlags f, bool *on) override {
    if (!on) return VROverlayError_InvalidParameter;
    *on = false;
    return with(h, [&](OverlayState &o) { *on = (o.flags & (uint32_t)f) != 0; });
  }
  E GetOverlayFlags(H h, uint32_t *flags) override { return get(h, flags, &OverlayState::flags); }
  E SetOverlayColor(H h, float r, float g, float b) override {
    return with(h, [&](OverlayState &o) { o.color[0] = r, o.color[1] = g, o.color[2] = b; });
  }
  E GetOverlayColor(H h, float *r, float *g, float *b) override {
    return with(h, [&](OverlayState &o) {
      if (r) *r = o.color[0];
      if (g) *g = o.color[1];
      if (b) *b = o.color[2];
    });
  }
  E SetOverlayAlpha(H h, float v) override { return set(h, v, &OverlayState::alpha); }
  E GetOverlayAlpha(H h, float *v) override { return get(h, v, &OverlayState::alpha); }
  E SetOverlayTexelAspect(H h, float v) override { return set(h, v, &OverlayState::texel_aspect); }
  E GetOverlayTexelAspect(H h, float *v) override { return get(h, v, &OverlayState::texel_aspect); }
  E SetOverlaySortOrder(H h, uint32_t v) override { return set(h, v, &OverlayState::sort_order); }
  E GetOverlaySortOrder(H h, uint32_t *v) override { return get(h, v, &OverlayState::sort_order); }
  E SetOverlayWidthInMeters(H h, float v) override { return set(h, v, &OverlayState::width); }
  E GetOverlayWidthInMeters(H h, float *v) override { return get(h, v, &OverlayState::width); }
  E SetOverlayCurvature(H h, float v) override { return set(h, v, &OverlayState::curvature); }
  E GetOverlayCurvature(H h, float *v) override { return get(h, v, &OverlayState::curvature); }
  E SetOverlayPreCurvePitch(H h, float v) override { return set(h, v, &OverlayState::pre_curve_pitch); }
  E GetOverlayPreCurvePitch(H h, float *v) override { return get(h, v, &OverlayState::pre_curve_pitch); }
  E SetOverlayTextureColorSpace(H h, EColorSpace v) override { return set(h, v, &OverlayState::color_space); }
  E GetOverlayTextureColorSpace(H h, EColorSpace *v) override { return get(h, v, &OverlayState::color_space); }
  E SetOverlayTextureBounds(H h, const VRTextureBounds_t *b) override {
    if (!b) return VROverlayError_InvalidParameter;
    return set(h, *b, &OverlayState::bounds);
  }
  E GetOverlayTextureBounds(H h, VRTextureBounds_t *b) override { return get(h, b, &OverlayState::bounds); }

  E GetOverlayTransformType(H h, VROverlayTransformType *t) override { return get(h, t, &OverlayState::transform_type); }
  E SetOverlayTransformAbsolute(H h, ETrackingUniverseOrigin origin, const HmdMatrix34_t *m) override {
    if (!m) return VROverlayError_InvalidParameter;
    return with(h, [&](OverlayState &o) {
      o.transform_type = VROverlayTransform_Absolute, o.origin = origin, o.transform = *m;
    });
  }
  E GetOverlayTransformAbsolute(H h, ETrackingUniverseOrigin *origin, HmdMatrix34_t *m) override {
    return with_type(h, VROverlayTransform_Absolute, [&](OverlayState &o) {
      if (origin) *origin = o.origin;
      if (m) *m = o.transform;
    });
  }
  E SetOverlayTransformTrackedDeviceRelative(H h, TrackedDeviceIndex_t dev, const HmdMatrix34_t *m) override {
    if (!m) return VROverlayError_InvalidParameter;
    if (dev >= k_unMaxTrackedDeviceCount) return VROverlayError_InvalidTrackedDevice;
    return with(h, [&](OverlayState &o) {
      o.transform_type = VROverlayTransform_TrackedDeviceRelative, o.device = dev, o.transform = *m;
    });
  }
  E GetOverlayTransformTrackedDeviceRelative(H h, TrackedDeviceIndex_t *dev, HmdMatrix34_t *m) override {
    return with_type(h, VROverlayTransform_TrackedDeviceRelative, [&](OverlayState &o) {
      if (dev) *dev = o.device;
      if (m) *m = o.transform;
    });
  }
  E SetOverlayTransformTrackedDeviceComponent(H h, TrackedDeviceIndex_t dev, const char *component) override {
    if (!component) return VROverlayError_InvalidParameter;
    if (dev >= k_unMaxTrackedDeviceCount) return VROverlayError_InvalidTrackedDevice;
    return with(h, [&](OverlayState &o) {
      o.transform_type = VROverlayTransform_TrackedComponent, o.device = dev, o.component = component;
    });
  }
  E GetOverlayTransformTrackedDeviceComponent(H h, TrackedDeviceIndex_t *dev, char *buf, uint32_t len) override {
    E too_small = VROverlayError_None;
    E e = with_type(h, VROverlayTransform_TrackedComponent, [&](OverlayState &o) {
      if (dev) *dev = o.device;
      if (!buf || len <= o.component.size()) too_small = VROverlayError_ArrayTooSmall;
      else memcpy(buf, o.component.c_str(), o.component.size() + 1);
    });
    return e != VROverlayError_None ? e : too_small;
  }
  E SetOverlayTransformCursor(H h, const HmdVector2_t *hotspot) override {
    if (!hotspot) return VROverlayError_InvalidParameter;
    return with(h, [&](OverlayState &o) { o.transform_type = VROverlayTransform_Cursor, o.cursor = *hotspot; });
  }
  E GetOverlayTransformCursor(H h, HmdVector2_t *hotspot) override {
    return with_type(h, VROverlayTransform_Cursor, [&](OverlayState &o) {
      if (hotspot) *hotspot = o.cursor;
    });
  }
  E SetOverlayTransformProjection(H h, ETrackingUniverseOrigin origin, const HmdMatrix34_t *m,
                                  const VROverlayProjection_t *, EVREye) override {
    if (!m) return VROverlayError_InvalidParameter;
    return with(h, [&](OverlayState &o) {
      o.transform_type = VROverlayTransform_Projection, o.origin = origin, o.transform = *m;
    });
  }
  E SetSubviewPosition(H h, float, float) override { return with(h, [](OverlayState &) {}); }

  E ShowOverlay(H h) override { return set(h, true, &OverlayState::visible); }
  E HideOverlay(H h) override { return set(h, false, &OverlayState::visible); }
  bool IsOverlayVisible(H h) override {
    bool v = false;
    with(h, [&](OverlayState &o) { v = o.visible && !o.dashboard && !o.thumbnail; });  // the dashboard is never open
    return v;
  }
  E GetTransformForOverlayCoordinates(H h, ETrackingUniverseOrigin, HmdVector2_t uv, HmdMatrix34_t *out) override {
    if (!out) return VROverlayError_InvalidParameter;
    return with_type(h, VROverlayTransform_Absolute, [&](OverlayState &o) {
      // flat quad of `width` metres centred on the transform; uv (0,0) is the lower-left texel
      float tw = o.tex_w ? (float)o.tex_w : 1, th = o.tex_h ? (float)o.tex_h : 1;
      float height = o.width * th / (tw * (o.texel_aspect > 0 ? o.texel_aspect : 1));
      float x = (uv.v[0] / tw - 0.5f) * o.width, y = (uv.v[1] / th - 0.5f) * height;
      *out = o.transform;
      for (int i = 0; i < 3; i++) out->m[i][3] += o.transform.m[i][0] * x + o.transform.m[i][1] * y;
    });
  }
  E WaitFrameSync(uint32_t) override { return VROverlayError_None; }
  bool PollNextOverlayEvent(H, VREvent_t *, uint32_t) override { return false; }
  E GetOverlayInputMethod(H h, VROverlayInputMethod *v) override { return get(h, v, &OverlayState::input); }
  E SetOverlayInputMethod(H h, VROverlayInputMethod v) override { return set(h, v, &OverlayState::input); }
  E GetOverlayMouseScale(H h, HmdVector2_t *v) override { return get(h, v, &OverlayState::mouse_scale); }
  E SetOverlayMouseScale(H h, const HmdVector2_t *v) override {
    if (!v) return VROverlayError_InvalidParameter;
    return set(h, *v, &OverlayState::mouse_scale);
  }
  bool ComputeOverlayIntersection(H, const VROverlayIntersectionParams_t *, VROverlayIntersectionResults_t *) override {
    return false;
  }
  bool IsHoverTargetOverlay(H) override { return false; }
  E SetOverlayIntersectionMask(H h, VROverlayIntersectionMaskPrimitive_t *, uint32_t, uint32_t) override { return ok(h); }
  E TriggerLaserMouseHapticVibration(H h, float, float, float) override { return ok(h); }
  E SetOverlayCursor(H h, H) override { return ok(h); }
  E SetOverlayCursorPositionOverride(H h, const HmdVector2_t *) override { return ok(h); }
  E ClearOverlayCursorPositionOverride(H h) override { return ok(h); }

  // The image is accepted and dropped; only its size is remembered (GetOverlayTextureSize, coordinates).
  E SetOverlayTexture(H h, const Texture_t *t) override {
    if (!t || !t->handle) return VROverlayError_InvalidTexture;
    return with(h, [&](OverlayState &o) { o.has_texture = true; });
  }
  E ClearOverlayTexture(H h) override {
    return with(h, [&](OverlayState &o) { o.has_texture = false, o.tex_w = o.tex_h = 0; });
  }
  E SetOverlayRaw(H h, void *buf, uint32_t w, uint32_t ht, uint32_t bpp) override {
    if (!buf || !w || !ht || bpp != 4) return VROverlayError_InvalidParameter;
    return with(h, [&](OverlayState &o) { o.has_texture = true, o.tex_w = w, o.tex_h = ht; });
  }
  E SetOverlayFromFile(H h, const char *path) override {
    if (!path) return VROverlayError_InvalidParameter;
    if (access(path, R_OK) != 0) return find_locked(h) ? VROverlayError_UnableToLoadFile : VROverlayError_InvalidHandle;
    return with(h, [&](OverlayState &o) { o.has_texture = true; });
  }
  E GetOverlayTexture(H h, void **native, void *, uint32_t *w, uint32_t *ht, uint32_t *fmt, ETextureType *,
                      EColorSpace *, VRTextureBounds_t *) override {
    if (native) *native = nullptr;
    if (w) *w = 0;
    if (ht) *ht = 0;
    if (fmt) *fmt = 0;
    return find_locked(h) ? VROverlayError_RequestFailed : VROverlayError_InvalidHandle;  // no shared textures
  }
  E ReleaseNativeOverlayHandle(H h, void *) override { return ok(h); }
  E GetOverlayTextureSize(H h, uint32_t *w, uint32_t *ht) override {
    return with(h, [&](OverlayState &o) {
      if (w) *w = o.tex_w;
      if (ht) *ht = o.tex_h;
    });
  }

  bool IsDashboardVisible() override { return false; }
  bool IsActiveDashboardOverlay(H) override { return false; }
  E SetDashboardOverlaySceneProcess(H h, uint32_t pid) override { return set(h, pid, &OverlayState::scene_pid); }
  E GetDashboardOverlaySceneProcess(H h, uint32_t *pid) override { return get(h, pid, &OverlayState::scene_pid); }
  void ShowDashboard(const char *) override {}
  TrackedDeviceIndex_t GetPrimaryDashboardDevice() override { return k_unTrackedDeviceIndexInvalid; }

  // no system keyboard: the request fails and the game keeps its own text entry
  E ShowKeyboard(EGamepadTextInputMode, EGamepadTextInputLineMode, uint32_t, const char *, uint32_t, const char *,
                 uint64_t) override {
    return VROverlayError_RequestFailed;
  }
  E ShowKeyboardForOverlay(H h, EGamepadTextInputMode, EGamepadTextInputLineMode, uint32_t, const char *, uint32_t,
                           const char *, uint64_t) override {
    return find_locked(h) ? VROverlayError_RequestFailed : VROverlayError_InvalidHandle;
  }
  uint32_t GetKeyboardText(char *buf, uint32_t len) override {
    if (buf && len) buf[0] = 0;
    return 0;
  }
  void HideKeyboard() override {}
  void SetKeyboardTransformAbsolute(ETrackingUniverseOrigin, const HmdMatrix34_t *) override {}
  void SetKeyboardPositionForOverlay(H, HmdRect2_t) override {}
  VRMessageOverlayResponse ShowMessageOverlay(const char *, const char *, const char *, const char *, const char *,
                                              const char *) override {
    return VRMessageOverlayResponse_CouldntFindSystemOverlay;
  }
  void CloseMessageOverlay() override {}

 private:
  E create(const char *key, const char *name, H *h) {
    if (h) *h = k_ulOverlayHandleInvalid;
    if (!key || !name || !h || !*key) return VROverlayError_InvalidParameter;
    if (strlen(key) >= k_unVROverlayMaxKeyLength) return VROverlayError_KeyTooLong;
    if (strlen(name) >= k_unVROverlayMaxNameLength) return VROverlayError_NameTooLong;
    for (auto &o : overlays_)
      if (o.second.key == key) return VROverlayError_KeyInUse;
    if (overlays_.size() >= kMaxOverlays) return VROverlayError_OverlayLimitExceeded;
    OverlayState o;
    o.key = key, o.name = name, o.pid = (uint32_t)getpid();
    *h = next_++;
    overlays_[*h] = o;
    return VROverlayError_None;
  }
  OverlayState *find(H h) {
    auto it = overlays_.find(h);
    return it == overlays_.end() ? nullptr : &it->second;
  }
  bool find_locked(H h) {
    std::lock_guard<std::mutex> g(m_);
    return find(h) != nullptr;
  }
  template <class F> E with(H h, F f, E result = VROverlayError_None) {
    std::lock_guard<std::mutex> g(m_);
    OverlayState *o = find(h);
    if (!o) return VROverlayError_InvalidHandle;
    f(*o);
    return result;
  }
  template <class F> E with_type(H h, VROverlayTransformType type, F f) {
    std::lock_guard<std::mutex> g(m_);
    OverlayState *o = find(h);
    if (!o) return VROverlayError_InvalidHandle;
    if (o->transform_type != type) return VROverlayError_WrongTransformType;
    f(*o);
    return VROverlayError_None;
  }
  E ok(H h) { return with(h, [](OverlayState &) {}); }
  template <class T> E set(H h, T v, T OverlayState::*field) {
    return with(h, [&](OverlayState &o) { o.*field = v; });
  }
  template <class T> E get(H h, T *v, T OverlayState::*field) {
    if (!v) return VROverlayError_InvalidParameter;
    return with(h, [&](OverlayState &o) { *v = o.*field; });
  }
  uint32_t str(H h, char *buf, uint32_t len, E *err, bool name) {
    uint32_t need = 0;
    E e = with(h, [&](OverlayState &o) {
      const std::string &s = name ? o.name : o.key;
      need = (uint32_t)s.size() + 1;
      if (buf && len >= need) memcpy(buf, s.c_str(), need);
    });
    if (e == VROverlayError_None && (!buf || len < need)) e = VROverlayError_ArrayTooSmall;
    if (err) *err = e;
    return need;
  }

  std::mutex m_;
  std::map<H, OverlayState> overlays_;
  std::map<H, H> thumbs_;  // dashboard overlay -> its thumbnail
  H next_ = 0x100000001ull;
};
}  // namespace

void *overlay_iface() {
  static Overlay o;
  return static_cast<vr::IVROverlay *>(&o);
}
}  // namespace svr
