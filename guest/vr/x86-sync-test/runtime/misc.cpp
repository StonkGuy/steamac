// SPDX-License-Identifier: Apache-2.0
// The small interfaces: IVRChaperone, IVRSettings, IVRApplications, IVRRenderModels, IVROverlay, IVRScreenshots,
// IVROverlayView. Behaviour = "a running compositor with nothing to show": calls succeed where an app treats an
// error as fatal, and report "not available" where apps cope with that (render models, overlays).
#include <string.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "interfaces.h"
#include "runtime.h"
#include "stubs.gen.h"

namespace svr {
namespace {
class Chaperone : public StubIVRChaperone {
 public:
  ChaperoneCalibrationState GetCalibrationState() override { return ChaperoneCalibrationState_OK; }
  bool GetPlayAreaSize(float *x, float *z) override {
    if (x) *x = 2.0f;
    if (z) *z = 2.0f;
    return true;
  }
  bool GetPlayAreaRect(HmdQuad_t *r) override {
    if (!r) return false;
    const float c[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int i = 0; i < 4; i++) r->vCorners[i] = HmdVector3_t{{c[i][0], 0, c[i][1]}};
    return true;
  }
  void ReloadInfo() override {}
  void SetSceneColor(HmdColor_t) override {}
  void GetBoundsColor(HmdColor_t *out, int n, float, HmdColor_t *cam) override {
    for (int i = 0; out && i < n; i++) out[i] = HmdColor_t{0, 0, 0, 0};
    if (cam) *cam = HmdColor_t{0, 0, 0, 0};
  }
  bool AreBoundsVisible() override { return false; }
  void ForceBoundsVisible(bool) override {}
  void ResetZeroPose(ETrackingUniverseOrigin) override {}
};

class Settings : public StubIVRSettings {
 public:
  const char *GetSettingsErrorNameFromEnum(EVRSettingsError e) override {
    return e == VRSettingsError_None ? "VRSettingsError_None" : "VRSettingsError_ReadFailed";
  }
  // writes are accepted and dropped; reads report "not set" so apps use their defaults
  void SetBool(const char *, const char *, bool, EVRSettingsError *e) override { ok(e); }
  void SetInt32(const char *, const char *, int32_t, EVRSettingsError *e) override { ok(e); }
  void SetFloat(const char *, const char *, float, EVRSettingsError *e) override { ok(e); }
  void SetString(const char *, const char *, const char *, EVRSettingsError *e) override { ok(e); }
  bool GetBool(const char *, const char *, EVRSettingsError *e) override { return unset(e), false; }
  int32_t GetInt32(const char *, const char *, EVRSettingsError *e) override { return unset(e), 0; }
  float GetFloat(const char *s, const char *k, EVRSettingsError *e) override {
    // the one value apps commonly read and need: the compositor's render scale
    if (s && k && !strcmp(s, k_pch_SteamVR_Section) && !strcmp(k, k_pch_SteamVR_SupersampleScale_Float)) {
      ok(e);
      return 1.0f;
    }
    return unset(e), 0.0f;
  }
  void GetString(const char *, const char *, char *buf, uint32_t len, EVRSettingsError *e) override {
    if (buf && len) buf[0] = 0;
    unset(e);
  }
  void RemoveSection(const char *, EVRSettingsError *e) override { ok(e); }
  void RemoveKeyInSection(const char *, const char *, EVRSettingsError *e) override { ok(e); }

 private:
  static void ok(EVRSettingsError *e) {
    if (e) *e = VRSettingsError_None;
  }
  static void unset(EVRSettingsError *e) {
    if (e) *e = VRSettingsError_UnsetSettingHasNoDefault;
  }
};

class Applications : public StubIVRApplications {
 public:
  EVRApplicationError AddApplicationManifest(const char *, bool) override { return VRApplicationError_None; }
  EVRApplicationError RemoveApplicationManifest(const char *) override { return VRApplicationError_None; }
  bool IsApplicationInstalled(const char *key) override { return key && Runtime::get().app_key == key; }
  uint32_t GetApplicationCount() override { return 0; }
  EVRApplicationError IdentifyApplication(uint32_t, const char *key) override {
    if (key) Runtime::get().app_key = key;
    return VRApplicationError_None;
  }
  uint32_t GetApplicationProcessId(const char *) override { return (uint32_t)getpid(); }
  const char *GetApplicationsErrorNameFromEnum(EVRApplicationError e) override {
    return e == VRApplicationError_None ? "VRApplicationError_None" : "VRApplicationError_UnknownApplication";
  }
  uint32_t GetApplicationPropertyString(const char *, EVRApplicationProperty, char *buf, uint32_t len,
                                        EVRApplicationError *e) override {
    if (buf && len) buf[0] = 0;
    if (e) *e = VRApplicationError_UnknownProperty;
    return 0;
  }
  bool GetApplicationPropertyBool(const char *, EVRApplicationProperty, EVRApplicationError *e) override {
    if (e) *e = VRApplicationError_UnknownProperty;
    return false;
  }
  uint64_t GetApplicationPropertyUint64(const char *, EVRApplicationProperty, EVRApplicationError *e) override {
    if (e) *e = VRApplicationError_UnknownProperty;
    return 0;
  }
  EVRApplicationError GetStartingApplication(char *buf, uint32_t len) override {
    if (buf && len) buf[0] = 0;
    return VRApplicationError_NoApplication;
  }
  EVRSceneApplicationState GetSceneApplicationState() override { return EVRSceneApplicationState_Running; }
  uint32_t GetCurrentSceneProcessId() override { return (uint32_t)getpid(); }
};

class RenderModels : public StubIVRRenderModels {
 public:
  // no model data: apps fall back to their own controller models
  EVRRenderModelError LoadRenderModel_Async(const char *, RenderModel_t **out) override {
    if (out) *out = nullptr;
    return VRRenderModelError_NotSupported;
  }
  void FreeRenderModel(RenderModel_t *) override {}
  EVRRenderModelError LoadTexture_Async(TextureID_t, RenderModel_TextureMap_t **out) override {
    if (out) *out = nullptr;
    return VRRenderModelError_NotSupported;
  }
  void FreeTexture(RenderModel_TextureMap_t *) override {}
  uint32_t GetRenderModelName(uint32_t, char *buf, uint32_t len) override {
    if (buf && len) buf[0] = 0;
    return 0;
  }
  uint32_t GetRenderModelCount() override { return 0; }
  uint32_t GetComponentCount(const char *) override { return 0; }
  bool RenderModelHasComponent(const char *, const char *) override { return false; }
  const char *GetRenderModelErrorNameFromEnum(vr::EVRRenderModelError) override { return "VRRenderModelError_NotSupported"; }
};

class Overlay : public StubIVROverlay {
 public:
  // no dashboard and no overlay compositor yet: overlay creation fails cleanly
  EVROverlayError FindOverlay(const char *, VROverlayHandle_t *h) override {
    if (h) *h = k_ulOverlayHandleInvalid;
    return VROverlayError_UnknownOverlay;
  }
  EVROverlayError CreateOverlay(const char *, const char *, VROverlayHandle_t *h) override {
    if (h) *h = k_ulOverlayHandleInvalid;
    return VROverlayError_RequestFailed;
  }
  EVROverlayError CreateDashboardOverlay(const char *, const char *, VROverlayHandle_t *h,
                                         VROverlayHandle_t *t) override {
    if (h) *h = k_ulOverlayHandleInvalid;
    if (t) *t = k_ulOverlayHandleInvalid;
    return VROverlayError_RequestFailed;
  }
  bool IsDashboardVisible() override { return false; }
  bool IsActiveDashboardOverlay(VROverlayHandle_t) override { return false; }
  const char *GetOverlayErrorNameFromEnum(EVROverlayError) override { return "VROverlayError_RequestFailed"; }
};

class Screenshots : public StubIVRScreenshots {};
class OverlayView : public StubIVROverlayView {};
}  // namespace

void *chaperone_iface() {
  static Chaperone c;
  return static_cast<vr::IVRChaperone *>(&c);
}
void *settings_iface() {
  static Settings s;
  return static_cast<vr::IVRSettings *>(&s);
}
void *applications_iface() {
  static Applications a;
  return static_cast<vr::IVRApplications *>(&a);
}
void *render_models_iface() {
  static RenderModels r;
  return static_cast<vr::IVRRenderModels *>(&r);
}
void *overlay_iface() {
  static Overlay o;
  return static_cast<vr::IVROverlay *>(&o);
}
void *screenshots_iface() {
  static Screenshots s;
  return static_cast<vr::IVRScreenshots *>(&s);
}
void *overlay_view_iface() {
  static OverlayView o;
  return static_cast<vr::IVROverlayView *>(&o);
}
}  // namespace svr
