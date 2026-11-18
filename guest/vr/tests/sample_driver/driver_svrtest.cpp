// SPDX-License-Identifier: Apache-2.0
// A SteamVR tracker driver written only against Valve's driver API (openvr_driver.h, in the style of Valve's
// samples/drivers), used to test svr-driverhost the way a real tracker driver or body estimator uses vrserver:
// VR_INIT_SERVER_DRIVER_CONTEXT, settings from its default.vrsettings, TrackedDeviceAdded from Init, properties written
// in Activate, GetRawTrackedDevicePoses for the headset, TrackedDevicePoseUpdated from RunFrame with a non-identity
// world-from-driver transform.
//   svrtest-waist       Prop_ControllerType_String vive_tracker_waist (role from the controller type)
//   svrtest-unlabelled  no controller type; role from the driver's "trackers" setting (TrackerRole_Chest)
// Both follow the headset: waist = head - (0, driver_svrtest/waist_drop_m, 0), chest = head - (0, 0.35, 0).
// Windows build: like a body estimator's driver (Standable's), it also serves a named pipe (\\.\pipe\svrtest) on which
// its companion application (tests/sample_driver/companion.c) sends body positions "x y z\n"; the waist then takes
// the latest one. That needs the driver and the application in one wineserver (svr-driverhost --then).
#include <math.h>
#include <string.h>

#include <string>

#include "openvr_driver.h"

#ifdef _WIN32
#include <stdio.h>
#include <windows.h>
#endif

using namespace vr;

namespace {
#ifdef _WIN32
// latest position from the companion application, as integers of millimetres (atomic without locks)
volatile LONG g_pipe_mm[3];
volatile LONG g_pipe_have = 0;
DWORD WINAPI pipe_server(void *) {
  for (;;) {
    HANDLE p = CreateNamedPipeA("\\\\.\\pipe\\svrtest", PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 0, 4096, 0,
                                nullptr);
    if (p == INVALID_HANDLE_VALUE) return 1;
    if (ConnectNamedPipe(p, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
      VRDriverLog()->Log("pipe: companion connected");
      char buf[256];
      DWORD n;
      std::string line;
      while (ReadFile(p, buf, sizeof buf, &n, nullptr) && n) {
        for (DWORD i = 0; i < n; i++) {
          if (buf[i] != '\n') {
            line += buf[i];
            continue;
          }
          float x, y, z;
          if (sscanf(line.c_str(), "%f %f %f", &x, &y, &z) == 3) {
            InterlockedExchange(&g_pipe_mm[0], (LONG)(x * 1000));
            InterlockedExchange(&g_pipe_mm[1], (LONG)(y * 1000));
            InterlockedExchange(&g_pipe_mm[2], (LONG)(z * 1000));
            InterlockedExchange(&g_pipe_have, 1);
          }
          line.clear();
        }
      }
    }
    CloseHandle(p);
  }
}
#endif

// world-from-driver: 90 degrees about +y, then 1 m along +x (the host must undo this)
const double kYaw = 1.5707963267948966, kTx = 1.0;

class Tracker final : public ITrackedDeviceServerDriver {
 public:
  Tracker(const char *serial, const char *type, float drop) : serial_(serial), type_(type), drop_(drop) {}
  EVRInitError Activate(uint32_t id) override {
    id_ = id;
    PropertyContainerHandle_t c = VRProperties()->TrackedDeviceToPropertyContainer(id);
    VRProperties()->SetStringProperty(c, Prop_ModelNumber_String, "svrtest tracker");
    VRProperties()->SetStringProperty(c, Prop_ManufacturerName_String, "svrtest");
    VRProperties()->SetStringProperty(c, Prop_TrackingSystemName_String, "svrtest");
    if (type_) VRProperties()->SetStringProperty(c, Prop_ControllerType_String, type_);
    VRProperties()->SetInt32Property(c, Prop_ControllerRoleHint_Int32, TrackedControllerRole_OptOut);
    VRDriverInput()->CreateBooleanComponent(c, "/input/system/click", &click_);
    return VRInitError_None;
  }
  void Deactivate() override { id_ = k_unTrackedDeviceIndexInvalid; }
  void EnterStandby() override {}
  void *GetComponent(const char *) override { return nullptr; }
  void DebugRequest(const char *, char *buf, uint32_t size) override {
    if (size) buf[0] = 0;
  }
  DriverPose_t GetPose() override { return DriverPose_t{}; }

  void RunFrame(const TrackedDevicePose_t &head) {
    if (id_ == k_unTrackedDeviceIndexInvalid) return;
    DriverPose_t p{};
    p.qWorldFromDriverRotation = {cos(kYaw / 2), 0, sin(kYaw / 2), 0};
    p.vecWorldFromDriverTranslation[0] = kTx;
    p.qDriverFromHeadRotation = {1, 0, 0, 0};
    p.qRotation = {1, 0, 0, 0};
    p.poseIsValid = head.bPoseIsValid;
    p.deviceIsConnected = true;
    p.result = TrackingResult_Running_OK;
#ifdef _WIN32
    if (g_pipe_have && !strcmp(serial_, "svrtest-waist")) {
      // the companion's position, world space: give it in driver space
      double wx = g_pipe_mm[0] / 1000.0 - kTx, wy = g_pipe_mm[1] / 1000.0, wz = g_pipe_mm[2] / 1000.0;
      double c = cos(-kYaw), s = sin(-kYaw);
      p.vecPosition[0] = c * wx + s * wz;
      p.vecPosition[1] = wy;
      p.vecPosition[2] = -s * wx + c * wz;
      p.qRotation = {cos(-kYaw / 2), 0, sin(-kYaw / 2), 0};
      p.poseIsValid = true;
      VRServerDriverHost()->TrackedDevicePoseUpdated(id_, p, sizeof(DriverPose_t));
      return;
    }
#endif
    if (head.bPoseIsValid) {
      // wanted world position, expressed in driver space: R^-1 (world - T)
      double wx = head.mDeviceToAbsoluteTracking.m[0][3] - kTx, wy = head.mDeviceToAbsoluteTracking.m[1][3] - drop_,
             wz = head.mDeviceToAbsoluteTracking.m[2][3];
      double c = cos(-kYaw), s = sin(-kYaw);
      p.vecPosition[0] = c * wx + s * wz;
      p.vecPosition[1] = wy;
      p.vecPosition[2] = -s * wx + c * wz;
      // and the device faces world forward: driver rotation = R^-1
      p.qRotation = {cos(-kYaw / 2), 0, sin(-kYaw / 2), 0};
    }
    VRServerDriverHost()->TrackedDevicePoseUpdated(id_, p, sizeof(DriverPose_t));
  }

  const char *serial_;
  const char *type_;
  float drop_;
  uint32_t id_ = k_unTrackedDeviceIndexInvalid;
  VRInputComponentHandle_t click_ = 0;
};

class Provider : public IServerTrackedDeviceProvider {
 public:
  EVRInitError Init(IVRDriverContext *ctx) override {
    VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
    if (!VRSettings()->GetBool("driver_svrtest", "enable")) return VRInitError_Init_NotInitialized;
    float drop = VRSettings()->GetFloat("driver_svrtest", "waist_drop_m");
    char path[512];
    VRResources()->GetResourceFullPath("{svrtest}/resources/settings/default.vrsettings", "", path, sizeof path);
    VRDriverLog()->Log(strstr(path, "svrtest/resources/settings/default.vrsettings") ? "resources ok" : "resources BAD");
    waist_ = new Tracker("svrtest-waist", "vive_tracker_waist", drop);
    chest_ = new Tracker("svrtest-unlabelled", nullptr, 0.35f);
    VRServerDriverHost()->TrackedDeviceAdded(waist_->serial_, TrackedDeviceClass_GenericTracker, waist_);
    VRServerDriverHost()->TrackedDeviceAdded(chest_->serial_, TrackedDeviceClass_GenericTracker, chest_);
#ifdef _WIN32
    CreateThread(nullptr, 0, pipe_server, nullptr, 0, nullptr);
#endif
    // a driver may also try to add a headset; the host refuses it
    hmd_ = new Tracker("svrtest-hmd", nullptr, 0);
    if (VRServerDriverHost()->TrackedDeviceAdded(hmd_->serial_, TrackedDeviceClass_HMD, hmd_))
      VRDriverLog()->Log("HMD accepted (unexpected)");
    return VRInitError_None;
  }
  void Cleanup() override {
    delete waist_;
    delete chest_;
    delete hmd_;
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
  }
  const char *const *GetInterfaceVersions() override { return k_InterfaceVersions; }
  void RunFrame() override {
    TrackedDevicePose_t poses[1];
    VRServerDriverHost()->GetRawTrackedDevicePoses(0, poses, 1);
    waist_->RunFrame(poses[0]);
    chest_->RunFrame(poses[0]);
  }
  bool ShouldBlockStandbyMode() override { return false; }
  void EnterStandby() override {}
  void LeaveStandby() override {}

  Tracker *waist_ = nullptr, *chest_ = nullptr, *hmd_ = nullptr;
};
Provider g_provider;
}  // namespace

#if defined(_WIN32)
#define SVRTEST_EXPORT extern "C" __declspec(dllexport)
#else
#define SVRTEST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

SVRTEST_EXPORT void *HmdDriverFactory(const char *name, int *ret) {
  if (!strcmp(name, IServerTrackedDeviceProvider_Version)) return &g_provider;
  if (ret) *ret = VRInitError_Init_InterfaceNotFound;
  return nullptr;
}
