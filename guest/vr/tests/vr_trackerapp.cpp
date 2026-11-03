// SPDX-License-Identifier: Apache-2.0
// vr-trackerapp: checks tracked-device table devices from a game's side of OpenVR (run_trackers.sh). Loads the
// runtime like Proton does and reads trackers the way VRChat-style titles do: device enumeration and classes through
// IVRSystem, string properties, activation events, and per-frame poses from WaitGetPoses' full device array.
//
//   vr-trackerapp RUNTIME_DIR feed N SECONDS      scene app: N svr-tracker-feed devices (standing-body layout around
//                                                 the head) appear, follow the head every frame, then go away
//                                                 (the source stops within SECONDS); expects one Deactivated each
//   vr-trackerapp RUNTIME_DIR driver SECONDS      scene app: the svrtest driver's two trackers through svr-driverhost
//   vr-trackerapp RUNTIME_DIR secondary           background app without a host link: the headset pose from the
//                                                 scene app's mirror, and the table's trackers
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>

#include "openvr.h"

namespace {
int failures = 0, checks = 0;
#define CHECK(cond, ...)                          \
  do {                                            \
    checks++;                                     \
    if (!(cond)) {                                \
      failures++;                                 \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
    }                                             \
  } while (0)

double now_s() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

std::string prop(vr::IVRSystem *sys, uint32_t i, vr::ETrackedDeviceProperty p) {
  char buf[256] = {};
  vr::ETrackedPropertyError e;
  sys->GetStringTrackedDeviceProperty(i, p, buf, sizeof buf, &e);
  return e == vr::TrackedProp_Success ? buf : "";
}

float px(const vr::TrackedDevicePose_t &p, int axis) { return p.mDeviceToAbsoluteTracking.m[axis][3]; }

// the head's yaw, as svr-tracker-feed computes it
float yaw_of(const vr::HmdMatrix34_t &m) {
  float fx = -m.m[0][2], fz = -m.m[2][2];
  return atan2f(-fx, -fz);
}

struct Expect {
  const char *type;
  float x, y, z, y_abs;  // head-yaw frame offsets, as kBody in svr-tracker-feed
};
const Expect kFeed[8] = {
    {"vive_tracker_waist", 0.0f, -0.70f, 0.05f, -1}, {"vive_tracker_left_foot", -0.12f, 0, 0.0f, 0.08f},
    {"vive_tracker_right_foot", 0.12f, 0, 0.0f, 0.08f}, {"vive_tracker_chest", 0.0f, -0.35f, 0.05f, -1},
    {"vive_tracker_left_knee", -0.10f, 0, -0.05f, 0.50f}, {"vive_tracker_right_knee", 0.10f, 0, -0.05f, 0.50f},
    {"vive_tracker_left_elbow", -0.25f, -0.50f, 0.10f, -1}, {"vive_tracker_right_elbow", 0.25f, -0.50f, 0.10f, -1},
};
}  // namespace

class IVRClientCore {
 public:
  virtual vr::EVRInitError Init(vr::EVRApplicationType, const char *) = 0;
  virtual void Cleanup() = 0;
  virtual vr::EVRInitError IsInterfaceVersionValid(const char *) = 0;
  virtual void *GetGenericInterface(const char *, vr::EVRInitError *) = 0;
  virtual bool BIsHmdPresent() = 0;
  virtual const char *GetEnglishStringForHmdError(vr::EVRInitError) = 0;
  virtual const char *GetIDForVRInitError(vr::EVRInitError) = 0;
};

int main(int argc, char **argv) {
  if (argc < 3) return fprintf(stderr, "usage: see the header of tests/vr_trackerapp.cpp\n"), 2;
  std::string runtime = argv[1], mode = argv[2];
  std::string so = runtime + "/bin/linux64/vrclient.so";
  void *lib = dlopen(so.c_str(), RTLD_NOW);
  if (!lib) return printf("FAIL dlopen %s: %s\n", so.c_str(), dlerror()), 1;
  auto factory = (void *(*)(const char *, int *))dlsym(lib, "VRClientCoreFactory");
  int rc = -1;
  auto *core = factory ? (IVRClientCore *)factory("IVRClientCore_003", &rc) : nullptr;
  if (!core) return printf("FAIL no client core\n"), 1;
  bool secondary = mode == "secondary";
  vr::EVRInitError err = core->Init(secondary ? vr::VRApplication_Background : vr::VRApplication_Scene, "");
  CHECK(err == vr::VRInitError_None, "Init (%s): %s", mode.c_str(), core->GetIDForVRInitError(err));
  if (err) return 1;
  auto *sys = (vr::IVRSystem *)core->GetGenericInterface(vr::IVRSystem_Version, &err);
  auto *comp = (vr::IVRCompositor *)core->GetGenericInterface(vr::IVRCompositor_Version, &err);
  if (!sys || !comp) return printf("FAIL interfaces\n"), 1;

  if (secondary) {
    // a companion app: no WaitGetPoses pacing of its own, it polls GetDeviceToAbsoluteTrackingPose
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    int valid = 0, trackers = 0;
    for (int i = 0; i < 20; i++) {
      sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
      if (poses[0].bPoseIsValid && fabsf(px(poses[0], 1) - 1.7f) < 0.05f) valid++;
      vr::TrackedDeviceIndex_t idx[16];
      trackers = (int)sys->GetSortedTrackedDeviceIndicesOfClass(vr::TrackedDeviceClass_GenericTracker, idx, 16);
      struct timespec ts = {0, 20000000};
      nanosleep(&ts, nullptr);
    }
    CHECK(sys->IsTrackedDeviceConnected(0), "secondary: HMD not connected");
    CHECK(valid >= 18, "secondary: head pose valid at standing height in %d of 20 polls", valid);
    CHECK(trackers == 3, "secondary: %d trackers (want the feed's 3)", trackers);
    printf("secondary: head valid %d/20, trackers %d\n", valid, trackers);
    core->Cleanup();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
  }

  bool driver = mode == "driver";
  int want = driver ? 2 : (argc > 3 ? atoi(argv[3]) : 3);
  double seconds = atof(driver ? (argc > 3 ? argv[3] : "4") : (argc > 4 ? argv[4] : "4"));
  if (seconds <= 0) seconds = 4;
  int activated = 0, deactivated = 0, frames = 0, frames_all = 0, moved = 0, max_seen = 0;
  float worst = 0, prev_x[16] = {};
  bool props_ok = false;
  double start = now_s(), first_all = 0;
  while (now_s() - start < seconds + 2.0) {
    vr::TrackedDevicePose_t render[vr::k_unMaxTrackedDeviceCount];
    CHECK(comp->WaitGetPoses(render, vr::k_unMaxTrackedDeviceCount, nullptr, 0) == vr::VRCompositorError_None, "wait");
    frames++;
    vr::VREvent_t ev;
    while (sys->PollNextEvent(&ev, sizeof ev)) {
      if (ev.trackedDeviceIndex < 3) continue;
      if (ev.eventType == vr::VREvent_TrackedDeviceActivated) activated++;
      if (ev.eventType == vr::VREvent_TrackedDeviceDeactivated) deactivated++;
    }
    vr::TrackedDeviceIndex_t idx[16];
    uint32_t n = sys->GetSortedTrackedDeviceIndicesOfClass(vr::TrackedDeviceClass_GenericTracker, idx, 16);
    if ((int)n > max_seen) max_seen = (int)n;
    if ((int)n != want) continue;
    if (!first_all) first_all = now_s();
    frames_all++;
    if (!props_ok) {
      // classes and properties a title reads once when a device appears
      props_ok = true;
      for (uint32_t k = 0; k < n; k++) {
        uint32_t i = idx[k];
        CHECK(i >= 3 && sys->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_GenericTracker, "class of %u", i);
        CHECK(sys->GetInt32TrackedDeviceProperty(i, vr::Prop_ControllerRoleHint_Int32) == vr::TrackedControllerRole_OptOut,
              "role hint of %u", i);
        CHECK(sys->GetControllerRoleForTrackedDeviceIndex(i) == vr::TrackedControllerRole_Invalid, "hand role of %u", i);
        CHECK(!prop(sys, i, vr::Prop_SerialNumber_String).empty(), "serial of %u", i);
        std::string type = prop(sys, i, vr::Prop_ControllerType_String);
        if (driver) {
          std::string serial = prop(sys, i, vr::Prop_SerialNumber_String);
          CHECK(prop(sys, i, vr::Prop_ModelNumber_String) == "svrtest tracker", "driver model of %u", i);
          CHECK(serial == "svrtest-waist" ? type == "vive_tracker_waist" : type == "vive_tracker_chest",
                "driver %s type %s", serial.c_str(), type.c_str());
        } else {
          CHECK(type == kFeed[k].type, "feed device %u type %s, want %s", i, type.c_str(), kFeed[k].type);
          CHECK(prop(sys, i, vr::Prop_InputProfilePath_String) == "{htc}/input/vive_tracker_profile.json", "profile");
        }
      }
    }
    // poses every frame: valid, in place around this frame's head (feed: within 5 cm, i.e. at most a source period
    // behind), and moving with the head
    const vr::TrackedDevicePose_t &head = render[0];
    float yaw = yaw_of(head.mDeviceToAbsoluteTracking), c = cosf(yaw), s = sinf(yaw);
    for (uint32_t k = 0; k < n; k++) {
      const vr::TrackedDevicePose_t &p = render[idx[k]];
      CHECK(p.bPoseIsValid && p.bDeviceIsConnected, "pose of %u invalid in frame %d", idx[k], frames);
      float ex, ey, ez;
      if (driver) {
        std::string serial = prop(sys, idx[k], vr::Prop_SerialNumber_String);
        ex = px(head, 0), ez = px(head, 2), ey = px(head, 1) - (serial == "svrtest-waist" ? 0.7f : 0.35f);
        // the driver's device faces world forward after the host undoes its world-from-driver rotation
        CHECK(fabsf(p.mDeviceToAbsoluteTracking.m[0][0] - 1) < 1e-3f && fabsf(p.mDeviceToAbsoluteTracking.m[2][2] - 1) < 1e-3f,
              "driver orientation of %u", idx[k]);
      } else {
        const Expect &e = kFeed[k];
        ex = px(head, 0) + c * e.x + s * e.z;
        ey = e.y_abs >= 0 ? e.y_abs : px(head, 1) + e.y;
        ez = px(head, 2) - s * e.x + c * e.z;
      }
      float d = sqrtf((px(p, 0) - ex) * (px(p, 0) - ex) + (px(p, 1) - ey) * (px(p, 1) - ey) + (px(p, 2) - ez) * (px(p, 2) - ez));
      // offsets only while the source surely runs (a killed source leaves its last poses for up to 1 s)
      if (d > worst && now_s() - start < seconds - 1.0) worst = d;
      if (fabsf(px(p, 0) - prev_x[k]) > 1e-5f) moved++;
      prev_x[k] = px(p, 0);
    }
  }
  printf("%s: max trackers %d, frames with all %d of %d, first after %.2f s, worst offset %.3f m, moved %d, "
         "activated %d deactivated %d\n",
         mode.c_str(), max_seen, frames_all, frames, first_all ? first_all - start : -1.0, worst, moved, activated,
         deactivated);
  CHECK(max_seen == want, "saw %d trackers, want %d", max_seen, want);
  CHECK(frames_all > 30, "only %d frames with all trackers", frames_all);
  CHECK(worst < (driver ? 0.08f : 0.05f), "tracker poses %.3f m away from the head-relative layout", worst);
  CHECK(driver || moved > frames_all, "trackers did not follow the head (moved %d)", moved);
  CHECK(activated == want, "activated %d", activated);
  CHECK(deactivated == want, "deactivated %d (the source stopped)", deactivated);
  uint32_t left = sys->GetSortedTrackedDeviceIndicesOfClass(vr::TrackedDeviceClass_GenericTracker, nullptr, 0);
  CHECK(left == 0, "%u trackers still connected after the source stopped", left);
  core->Cleanup();
  printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
