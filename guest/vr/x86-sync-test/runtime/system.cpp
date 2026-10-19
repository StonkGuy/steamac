// SPDX-License-Identifier: Apache-2.0
// IVRSystem: headset geometry, tracked devices and their properties, events, legacy controller state.
#include <string.h>

#include <cmath>
#include <string>

#include "interfaces.h"
#include "runtime.h"
#include "stubs.gen.h"
#include "vrmath.h"

namespace svr {
namespace {
uint32_t copy_string(const std::string &s, char *buf, uint32_t size, vr::ETrackedPropertyError *err) {
  uint32_t need = (uint32_t)s.size() + 1;
  if (buf && size >= need) {
    memcpy(buf, s.c_str(), need);
    if (err) *err = vr::TrackedProp_Success;
  } else if (err) {
    *err = vr::TrackedProp_BufferTooSmall;
  }
  return need;
}

template <typename T>
T prop_result(vr::ETrackedPropertyError *err, vr::ETrackedPropertyError e, T v) {
  if (err) *err = e;
  return v;
}

class System : public StubIVRSystem {
 public:
  void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) override {
    svr_hello_rep hi = Runtime::get().hello();
    if (w) *w = hi.eye_width ? hi.eye_width : 1832;
    if (h) *h = hi.eye_height ? hi.eye_height : 1920;
  }

  void GetProjectionRaw(EVREye eye, float *l, float *r, float *t, float *b) override {
    svr_hello_rep hi = Runtime::get().hello();
    const float *f = hi.fov[eye == Eye_Right ? 1 : 0];  // left, right, up, down (radians)
    // OpenVR: tangents, y down (top is negative for an upward angle)
    if (l) *l = std::tan(f[0]);
    if (r) *r = std::tan(f[1]);
    if (t) *t = -std::tan(f[2]);
    if (b) *b = -std::tan(f[3]);
  }

  HmdMatrix44_t GetProjectionMatrix(EVREye eye, float zn, float zf) override {
    float l, r, t, b;
    GetProjectionRaw(eye, &l, &r, &t, &b);
    return projection(l, r, t, b, zn, zf);
  }

  bool ComputeDistortion(EVREye, float u, float v, DistortionCoordinates_t *d) override {
    // the headset's own runtime undistorts the rectilinear stream: identity here
    if (d)
      for (int i = 0; i < 2; i++) {
        d->rfRed[i] = d->rfGreen[i] = d->rfBlue[i] = i ? v : u;
      }
    return true;
  }

  HmdMatrix34_t GetEyeToHeadTransform(EVREye eye) override {
    float ipd = Runtime::get().hello().ipd_m;
    if (ipd <= 0) ipd = 0.063f;
    static const float q[4] = {0, 0, 0, 1};
    float p[3] = {(eye == Eye_Left ? -0.5f : 0.5f) * ipd, 0, 0};
    return to_matrix34(q, p);
  }

  bool GetTimeSinceLastVsync(float *seconds, uint64_t *frame) override {
    Runtime &rt = Runtime::get();
    int64_t v = rt.last_vsync_guest();
    if (seconds) *seconds = v ? (float)((now_ns() - v) * 1e-9) : 0.0f;
    if (frame) *frame = rt.frame_id();
    return true;
  }

  int32_t GetD3D9AdapterIndex() override { return 0; }
  void GetDXGIOutputInfo(int32_t *adapter) override {
    if (adapter) *adapter = 0;  // Proton maps this to the adapter the game already uses
  }
  void GetOutputDevice(uint64_t *dev, ETextureType, VkInstance_T *) override {
    if (dev) *dev = 0;
  }
  bool IsDisplayOnDesktop() override { return false; }
  bool SetDisplayVisibility(bool) override { return false; }

  void GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin, float seconds, TrackedDevicePose_t *poses,
                                       uint32_t count) override {
    Runtime &rt = Runtime::get();
    rt.poll();
    for (uint32_t i = 0; i < count; i++) poses[i] = rt.device_pose(i, seconds);
  }

  HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() override { return identity34(); }
  HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() override { return identity34(); }

  uint32_t GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass cls, vr::TrackedDeviceIndex_t *out, uint32_t size,
                                                vr::TrackedDeviceIndex_t) override {
    uint32_t n = 0;
    for (vr::TrackedDeviceIndex_t i = 0; i < kDeviceCount; i++) {
      if (GetTrackedDeviceClass(i) != cls || !IsTrackedDeviceConnected(i)) continue;
      if (out && n < size) out[n] = i;
      n++;
    }
    return n;
  }

  EDeviceActivityLevel GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t) override {
    return k_EDeviceActivityLevel_UserInteraction;
  }

  void ApplyTransform(TrackedDevicePose_t *out, const TrackedDevicePose_t *in, const HmdMatrix34_t *t) override {
    *out = *in;
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 4; c++) {
        float v = c == 3 ? t->m[r][3] : 0;
        for (int k = 0; k < 3; k++) v += t->m[r][k] * in->mDeviceToAbsoluteTracking.m[k][c];
        out->mDeviceToAbsoluteTracking.m[r][c] = v;
      }
  }

  vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole role) override {
    if (role == TrackedControllerRole_LeftHand) return kLeft;
    if (role == TrackedControllerRole_RightHand) return kRight;
    return k_unTrackedDeviceIndexInvalid;
  }
  vr::ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t i) override {
    return i == kLeft ? TrackedControllerRole_LeftHand : i == kRight ? TrackedControllerRole_RightHand
                                                                     : TrackedControllerRole_Invalid;
  }
  ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t i) override {
    return i == kHmd ? TrackedDeviceClass_HMD : (i == kLeft || i == kRight) ? TrackedDeviceClass_Controller
                                                                            : TrackedDeviceClass_Invalid;
  }
  bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t i) override { return Runtime::get().device_connected(i); }

  bool GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t i, ETrackedDeviceProperty p,
                                    ETrackedPropertyError *e) override {
    if (i >= kDeviceCount) return prop_result(e, TrackedProp_InvalidDevice, false);
    switch (p) {
      case Prop_DeviceProvidesBatteryStatus_Bool: return prop_result(e, TrackedProp_Success, false);
      case Prop_DeviceIsWireless_Bool: return prop_result(e, TrackedProp_Success, true);
      case Prop_DeviceIsCharging_Bool: return prop_result(e, TrackedProp_Success, false);
      case Prop_HasDisplayComponent_Bool: return prop_result(e, TrackedProp_Success, i == kHmd);
      case Prop_HasControllerComponent_Bool: return prop_result(e, TrackedProp_Success, i != kHmd);
      default: return prop_result(e, TrackedProp_UnknownProperty, false);
    }
  }

  float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t i, ETrackedDeviceProperty p,
                                      ETrackedPropertyError *e) override {
    if (i >= kDeviceCount) return prop_result(e, TrackedProp_InvalidDevice, 0.0f);
    switch (p) {
      case Prop_DisplayFrequency_Float:
        if (i == kHmd) return prop_result(e, TrackedProp_Success, Runtime::get().refresh_hz());
        break;
      case Prop_UserIpdMeters_Float:
        if (i == kHmd) {
          float ipd = Runtime::get().hello().ipd_m;
          return prop_result(e, TrackedProp_Success, ipd > 0 ? ipd : 0.063f);
        }
        break;
      case Prop_SecondsFromVsyncToPhotons_Float:
        if (i == kHmd) return prop_result(e, TrackedProp_Success, 0.0f);
        break;
      case Prop_DeviceBatteryPercentage_Float: return prop_result(e, TrackedProp_Success, 1.0f);
      default: break;
    }
    return prop_result(e, TrackedProp_UnknownProperty, 0.0f);
  }

  int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t i, ETrackedDeviceProperty p,
                                        ETrackedPropertyError *e) override {
    if (i >= kDeviceCount) return prop_result(e, TrackedProp_InvalidDevice, 0);
    switch (p) {
      case Prop_DeviceClass_Int32: return prop_result(e, TrackedProp_Success, (int32_t)GetTrackedDeviceClass(i));
      case Prop_ControllerRoleHint_Int32:
        if (i != kHmd)
          return prop_result(e, TrackedProp_Success, (int32_t)GetControllerRoleForTrackedDeviceIndex(i));
        break;
      case Prop_Axis0Type_Int32:
        if (i != kHmd) return prop_result(e, TrackedProp_Success, (int32_t)k_eControllerAxis_Joystick);
        break;
      case Prop_Axis1Type_Int32:
      case Prop_Axis2Type_Int32:
        if (i != kHmd) return prop_result(e, TrackedProp_Success, (int32_t)k_eControllerAxis_Trigger);
        break;
      default: break;
    }
    return prop_result(e, TrackedProp_UnknownProperty, 0);
  }

  uint64_t GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t i, ETrackedDeviceProperty p,
                                          ETrackedPropertyError *e) override {
    if (i >= kDeviceCount) return prop_result(e, TrackedProp_InvalidDevice, (uint64_t)0);
    switch (p) {
      case Prop_CurrentUniverseId_Uint64: return prop_result(e, TrackedProp_Success, (uint64_t)1);
      case Prop_SupportedButtons_Uint64:
        if (i != kHmd)
          return prop_result(e, TrackedProp_Success,
                             ButtonMaskFromId(k_EButton_System) | ButtonMaskFromId(k_EButton_ApplicationMenu) |
                                 ButtonMaskFromId(k_EButton_Grip) | ButtonMaskFromId(k_EButton_A) |
                                 ButtonMaskFromId(k_EButton_Axis0) | ButtonMaskFromId(k_EButton_Axis1) |
                                 ButtonMaskFromId(k_EButton_Axis2));
        break;
      default: break;
    }
    return prop_result(e, TrackedProp_UnknownProperty, (uint64_t)0);
  }

  HmdMatrix34_t GetMatrix34TrackedDeviceProperty(vr::TrackedDeviceIndex_t, ETrackedDeviceProperty,
                                                 ETrackedPropertyError *e) override {
    return prop_result(e, TrackedProp_UnknownProperty, identity34());
  }

  uint32_t GetArrayTrackedDeviceProperty(vr::TrackedDeviceIndex_t, ETrackedDeviceProperty, PropertyTypeTag_t, void *,
                                         uint32_t, ETrackedPropertyError *e) override {
    return prop_result(e, TrackedProp_UnknownProperty, 0u);
  }

  uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t i, ETrackedDeviceProperty p, char *buf,
                                          uint32_t size, ETrackedPropertyError *e) override {
    if (i >= kDeviceCount) return prop_result(e, TrackedProp_InvalidDevice, 0u);
    if (i == kHmd) {
      svr_hello_rep hi = Runtime::get().hello();
      switch (p) {
        case Prop_TrackingSystemName_String: return copy_string("steamac", buf, size, e);
        case Prop_ModelNumber_String: return copy_string(hi.name[0] ? hi.name : "steamac HMD", buf, size, e);
        case Prop_ManufacturerName_String: return copy_string("steamac", buf, size, e);
        case Prop_SerialNumber_String: return copy_string("steamac-hmd", buf, size, e);
        case Prop_RenderModelName_String: return copy_string("generic_hmd", buf, size, e);
        default: return prop_result(e, TrackedProp_UnknownProperty, 0u);
      }
    }
    ControllerInfo ci = controller_info(i == kLeft ? 0 : 1);
    switch (p) {
      case Prop_TrackingSystemName_String: return copy_string(ci.tracking_system, buf, size, e);
      case Prop_ModelNumber_String: return copy_string(ci.model, buf, size, e);
      case Prop_ManufacturerName_String: return copy_string(ci.manufacturer, buf, size, e);
      case Prop_SerialNumber_String: return copy_string(i == kLeft ? "steamac-left" : "steamac-right", buf, size, e);
      case Prop_RenderModelName_String: return copy_string(ci.render_model, buf, size, e);
      case Prop_ControllerType_String: return copy_string(ci.controller_type, buf, size, e);
      case Prop_InputProfilePath_String: return copy_string(ci.input_profile, buf, size, e);
      default: return prop_result(e, TrackedProp_UnknownProperty, 0u);
    }
  }

  const char *GetPropErrorNameFromEnum(ETrackedPropertyError err) override {
    switch (err) {
      case TrackedProp_Success: return "TrackedProp_Success";
      case TrackedProp_UnknownProperty: return "TrackedProp_UnknownProperty";
      case TrackedProp_InvalidDevice: return "TrackedProp_InvalidDevice";
      case TrackedProp_BufferTooSmall: return "TrackedProp_BufferTooSmall";
      default: return "TrackedProp_Error";
    }
  }

  bool PollNextEvent(VREvent_t *ev, uint32_t size) override { return Runtime::get().pop_event(ev, size); }
  bool PollNextEventWithPose(ETrackingUniverseOrigin o, VREvent_t *ev, uint32_t size, TrackedDevicePose_t *pose) override {
    if (!Runtime::get().pop_event(ev, size)) return false;
    if (pose) *pose = Runtime::get().device_pose(ev->trackedDeviceIndex, 0);
    (void)o;
    return true;
  }
  bool PollNextEventWithPoseAndOverlays(ETrackingUniverseOrigin o, VREvent_t *ev, uint32_t size,
                                        TrackedDevicePose_t *pose, VROverlayHandle_t *overlay) override {
    if (overlay) *overlay = k_ulOverlayHandleInvalid;
    return PollNextEventWithPose(o, ev, size, pose);
  }

  HiddenAreaMesh_t GetHiddenAreaMesh(EVREye, EHiddenAreaMeshType) override { return HiddenAreaMesh_t{nullptr, 0}; }

  bool GetControllerState(vr::TrackedDeviceIndex_t i, vr::VRControllerState_t *st, uint32_t size) override {
    if ((i != kLeft && i != kRight) || !st || size < sizeof(vr::VRControllerState001_t)) return false;
    Runtime::get().poll();
    return legacy_controller_state(i == kLeft ? 0 : 1, st);
  }
  bool GetControllerStateWithPose(ETrackingUniverseOrigin, vr::TrackedDeviceIndex_t i, vr::VRControllerState_t *st,
                                  uint32_t size, TrackedDevicePose_t *pose) override {
    if (pose) *pose = Runtime::get().device_pose(i, 0);
    return GetControllerState(i, st, size);
  }
  void TriggerHapticPulse(vr::TrackedDeviceIndex_t i, uint32_t, unsigned short us) override {
    if (i == kLeft || i == kRight) Runtime::get().haptic(i == kLeft ? 0 : 1, us * 1e-6f, 0, 1.0f);
  }

  bool IsInputAvailable() override { return true; }
  bool IsSteamVRDrawingControllers() override { return false; }
  bool ShouldApplicationPause() override { return false; }
  bool ShouldApplicationReduceRenderingWork() override { return false; }
  void AcknowledgeQuit_Exiting() override {}
  const char *GetRuntimeVersion() override { return "steamac-vr 0.1"; }
  vr::EVRInitError SetSDKVersion(uint32_t, uint32_t, uint32_t) override { return VRInitError_None; }
};
}  // namespace

void *system_iface() {
  static System s;
  return static_cast<vr::IVRSystem *>(&s);
}

ControllerInfo controller_info(int hand) {
  bool index = Runtime::get().profile() == SVR_PROFILE_INDEX;
  if (index)
    return {"knuckles",
            hand ? "Knuckles Right" : "Knuckles Left",
            hand ? "{indexcontroller}valve_controller_knu_1_0_right" : "{indexcontroller}valve_controller_knu_1_0_left",
            "lighthouse",
            "Valve",
            "{indexcontroller}/input/index_controller_profile.json"};
  return {"oculus_touch",
          hand ? "Oculus Quest2 (Right Controller)" : "Oculus Quest2 (Left Controller)",
          hand ? "oculus_quest2_controller_right" : "oculus_quest2_controller_left",
          "oculus",
          "Oculus",
          "{oculus}/input/touch_profile.json"};
}

bool legacy_controller_state(int hand, vr::VRControllerState_t *st) {
  Runtime &rt = Runtime::get();
  memset(st, 0, sizeof(vr::VRControllerState001_t));
  auto in = [&](int id) { return rt.input(hand, id); };
  st->unPacketNum = (uint32_t)rt.frame_id();
  auto press = [&](vr::EVRButtonId b, bool on) {
    if (on) st->ulButtonPressed |= vr::ButtonMaskFromId(b);
  };
  auto touch = [&](vr::EVRButtonId b, bool on) {
    if (on) st->ulButtonTouched |= vr::ButtonMaskFromId(b);
  };
  press(vr::k_EButton_ApplicationMenu, in(SVR_IN_B_CLICK) > 0.5f || in(SVR_IN_MENU_CLICK) > 0.5f);
  touch(vr::k_EButton_ApplicationMenu, in(SVR_IN_B_TOUCH) > 0.5f);
  press(vr::k_EButton_A, in(SVR_IN_A_CLICK) > 0.5f);
  touch(vr::k_EButton_A, in(SVR_IN_A_TOUCH) > 0.5f);
  press(vr::k_EButton_Grip, in(SVR_IN_SQUEEZE_CLICK) > 0.5f || in(SVR_IN_SQUEEZE_VALUE) > 0.7f);
  press(vr::k_EButton_Axis0, in(SVR_IN_THUMBSTICK_CLICK) > 0.5f);
  touch(vr::k_EButton_Axis0, in(SVR_IN_THUMBSTICK_TOUCH) > 0.5f);
  press(vr::k_EButton_Axis1, in(SVR_IN_TRIGGER_CLICK) > 0.5f);
  touch(vr::k_EButton_Axis1, in(SVR_IN_TRIGGER_TOUCH) > 0.5f);
  st->rAxis[0].x = in(SVR_IN_THUMBSTICK_X);
  st->rAxis[0].y = in(SVR_IN_THUMBSTICK_Y);
  st->rAxis[1].x = in(SVR_IN_TRIGGER_VALUE);
  st->rAxis[2].x = in(SVR_IN_SQUEEZE_VALUE);
  return rt.device_connected(hand ? kRight : kLeft);
}
}  // namespace svr
