// SPDX-License-Identifier: Apache-2.0
// IVRCompositor: frame pacing (WaitGetPoses), eye texture submit, timing.
#include <string.h>

#include "interfaces.h"
#include "runtime.h"
#include "stubs.gen.h"
#include "vrmath.h"

namespace svr {
namespace {
class Compositor : public StubIVRCompositor {
 public:
  void SetTrackingSpace(ETrackingUniverseOrigin o) override { origin_ = o; }
  ETrackingUniverseOrigin GetTrackingSpace() override { return origin_; }

  EVRCompositorError WaitGetPoses(TrackedDevicePose_t *render, uint32_t nrender, TrackedDevicePose_t *game,
                                  uint32_t ngame) override {
    Runtime &rt = Runtime::get();
    rt.pose_stats().wait++;
    if (!rt.wait_frame()) {
      rt.pose_stats().wait_fail++;
      return VRCompositorError_DoNotHaveFocus;
    }
    frame_start_ = now_ns();
    fill(render, nrender);
    if (render && nrender) rt.note_render_head(render[kHmd]);
    // "game" poses are predicted one frame further (SteamVR semantics); same snapshot, extrapolated
    if (game) {
      int64_t t = rt.frame_display_guest() + (int64_t)(1e9f / rt.refresh_hz());
      for (uint32_t i = 0; i < ngame; i++) {
        int d = i == kHmd ? SVR_DEV_HEAD : i == kLeft ? SVR_DEV_LEFT_GRIP : i == kRight ? SVR_DEV_RIGHT_GRIP : -1;
        game[i] = d < 0 ? to_openvr(svr_pose{}, false) : to_openvr(rt.pose_at(d, t), rt.device_connected(i));
      }
    }
    return VRCompositorError_None;
  }

  EVRCompositorError GetLastPoses(TrackedDevicePose_t *render, uint32_t nrender, TrackedDevicePose_t *game,
                                  uint32_t ngame) override {
    Runtime::get().pose_stats().last_poses++;
    fill(render, nrender);
    fill(game, ngame);
    return VRCompositorError_None;
  }

  EVRCompositorError GetLastPoseForTrackedDeviceIndex(TrackedDeviceIndex_t i, TrackedDevicePose_t *out,
                                                      TrackedDevicePose_t *game) override {
    if (i >= k_unMaxTrackedDeviceCount) return VRCompositorError_IndexOutOfRange;
    Runtime::get().pose_stats().last_pose_index++;
    vr::TrackedDevicePose_t p = Runtime::get().device_frame_pose(i);
    if (out) *out = p;
    if (game) *game = p;
    return VRCompositorError_None;
  }

  EVRCompositorError Submit(EVREye eye, const Texture_t *tex, const VRTextureBounds_t *bounds,
                            EVRSubmitFlags flags) override {
    return Runtime::get().submit(eye, tex, bounds, flags);
  }
  EVRCompositorError SubmitWithArrayIndex(EVREye eye, const Texture_t *tex, uint32_t slice,
                                          const VRTextureBounds_t *bounds, EVRSubmitFlags flags) override {
    return Runtime::get().submit(eye, tex, bounds, flags, slice);
  }
  void ClearLastSubmittedFrame() override { Runtime::get().clear_frame(); }
  void PostPresentHandoff() override { Runtime::get().post_present_handoff(); }

  bool GetFrameTiming(Compositor_FrameTiming *t, uint32_t frames_ago) override {
    if (!t || t->m_nSize < sizeof(uint32_t) * 2) return false;
    uint32_t size = t->m_nSize;
    memset(t, 0, size < sizeof(*t) ? size : sizeof(*t));
    t->m_nSize = size;
    Runtime &rt = Runtime::get();
    t->m_nFrameIndex = (uint32_t)(rt.frame_id() - frames_ago);
    t->m_nNumFramePresents = 1;
    t->m_nReprojectionFlags = 0;
    if (size >= offsetof(Compositor_FrameTiming, m_flSystemTimeInSeconds) + sizeof(double))
      t->m_flSystemTimeInSeconds = frame_start_ * 1e-9;
    return true;
  }
  uint32_t GetFrameTimings(Compositor_FrameTiming *t, uint32_t n) override {
    // older versions of the struct are smaller: the array stride is the caller's m_nSize, not sizeof
    if (!t || !n) return 0;
    uint32_t stride = t->m_nSize;
    if (stride < sizeof(uint32_t) * 2 || stride > sizeof(*t)) return 0;
    for (uint32_t i = 0; i < n; i++) {
      auto *e = (Compositor_FrameTiming *)((char *)t + (size_t)stride * i);
      e->m_nSize = stride;
      if (!GetFrameTiming(e, i)) return i;
    }
    return n;
  }
  float GetFrameTimeRemaining() override {
    float r = (float)((Runtime::get().frame_display_guest() - now_ns()) * 1e-9);
    return r > 0 ? r : 0;
  }
  void GetCumulativeStats(Compositor_CumulativeStats *s, uint32_t size) override {
    if (!s) return;
    memset(s, 0, size < sizeof(*s) ? size : sizeof(*s));
    s->m_nNumFramePresents = Runtime::get().frames_presented();
  }

  void FadeToColor(float, float, float, float, float, bool) override {}
  HmdColor_t GetCurrentFadeColor(bool) override { return HmdColor_t{0, 0, 0, 0}; }
  void FadeGrid(float, bool) override {}
  // the skybox shows while the application is not submitting (level loads); the host keeps the last frame
  EVRCompositorError SetSkyboxOverride(const Texture_t *, uint32_t) override { return VRCompositorError_None; }
  void ClearSkyboxOverride() override {}
  float GetCurrentGridAlpha() override { return 0; }
  void CompositorBringToFront() override {}
  void CompositorGoToBack() override {}
  bool IsFullscreen() override { return true; }
  uint32_t GetCurrentSceneFocusProcess() override { return 0; }
  uint32_t GetLastFrameRenderer() override { return 0; }
  bool CanRenderScene() override { return true; }
  void ShowMirrorWindow() override {}
  void HideMirrorWindow() override {}
  bool IsMirrorWindowVisible() override { return false; }
  bool ShouldAppRenderWithLowResources() override { return false; }
  void ForceInterleavedReprojectionOn(bool) override {}
  void SuspendRendering(bool) override {}

  uint32_t GetVulkanInstanceExtensionsRequired(char *buf, uint32_t size) override { return empty(buf, size); }
  uint32_t GetVulkanDeviceExtensionsRequired(VkPhysicalDevice_T *, char *buf, uint32_t size) override {
    return empty(buf, size);
  }

  void SetExplicitTimingMode(EVRCompositorTimingMode) override {}
  EVRCompositorError SubmitExplicitTimingData() override { return VRCompositorError_None; }
  bool IsMotionSmoothingEnabled() override { return false; }
  bool IsMotionSmoothingSupported() override { return false; }
  bool IsCurrentSceneFocusAppLoading() override { return false; }

 private:
  static uint32_t empty(char *buf, uint32_t size) {
    if (buf && size) buf[0] = 0;
    return 1;
  }
  void fill(TrackedDevicePose_t *p, uint32_t n) {
    if (!p) return;
    for (uint32_t i = 0; i < n; i++) p[i] = Runtime::get().device_frame_pose(i);
  }

  ETrackingUniverseOrigin origin_ = TrackingUniverseStanding;
  int64_t frame_start_ = 0;
};
}  // namespace

void *compositor_iface() {
  static Compositor c;
  return static_cast<vr::IVRCompositor *>(&c);
}
}  // namespace svr
