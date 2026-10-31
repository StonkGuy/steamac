// SPDX-License-Identifier: Apache-2.0
// vr-testapp: an OpenVR client that loads a runtime the way Proton's vrclient unix side does
// (dlopen <runtime>/bin/linux64/vrclient.so, VRClientCoreFactory("IVRClientCore_003")) and checks poses,
// frame pacing, submits, SteamVR Input actions, haptics and the legacy controller API end to end.
// Pair it with vr-fakehost and tests/data/script.txt (run_e2e.sh); exits 0 when every check passes.
//
//   vr-testapp RUNTIME_DIR DATA_DIR [--frames N] [--profile touch|index]
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "openvr.h"
#include "steamac_vr_guest.h"

namespace {
int failures = 0, checks = 0;
#define CHECK(cond, ...)                         \
  do {                                           \
    checks++;                                    \
    if (!(cond)) {                               \
      failures++;                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                       \
      printf("\n");                              \
    }                                            \
  } while (0)

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

float yaw_of(const vr::HmdMatrix34_t &m) { return atan2f(m.m[0][2], m.m[2][2]); }
bool near(float a, float b, float eps) { return fabsf(a - b) <= eps; }
}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: vr-testapp RUNTIME_DIR DATA_DIR [--frames N] [--profile touch|index]\n");
    return 2;
  }
  std::string runtime = argv[1], data = argv[2];
  int frames = 150;
  bool index = false;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--profile") && i + 1 < argc) index = !strcmp(argv[++i], "index");
  }

  std::string so = runtime + "/bin/linux64/vrclient.so";
  void *lib = dlopen(so.c_str(), RTLD_NOW);
  if (!lib) return printf("FAIL dlopen %s: %s\n", so.c_str(), dlerror()), 1;
  auto factory = (void *(*)(const char *, int *))dlsym(lib, "VRClientCoreFactory");
  if (!factory) return printf("FAIL no VRClientCoreFactory\n"), 1;
  int rc = -1;
  auto *core = (IVRClientCore *)factory("IVRClientCore_003", &rc);
  CHECK(core && rc == 0, "factory returned %p rc %d", (void *)core, rc);
  if (!core) return 1;
  vr::EVRInitError err = core->Init(vr::VRApplication_Scene, "");
  CHECK(err == vr::VRInitError_None, "Init: %s", core->GetIDForVRInitError(err));
  if (err) return 1;

  auto *sys = (vr::IVRSystem *)core->GetGenericInterface(vr::IVRSystem_Version, &err);
  auto *comp = (vr::IVRCompositor *)core->GetGenericInterface(vr::IVRCompositor_Version, &err);
  auto *input = (vr::IVRInput *)core->GetGenericInterface(vr::IVRInput_Version, &err);
  CHECK(sys && comp && input, "core interfaces missing");
  CHECK(!core->GetGenericInterface("IVRSystem_001", &err) && err == vr::VRInitError_Init_InterfaceNotFound,
        "unknown version must be refused");
  CHECK(core->GetGenericInterface(vr::IVRChaperone_Version, &err) && core->GetGenericInterface(vr::IVRSettings_Version, &err) &&
            core->GetGenericInterface(vr::IVRApplications_Version, &err) &&
            core->GetGenericInterface(vr::IVRRenderModels_Version, &err),
        "VRChat's auxiliary interfaces missing");
  if (!sys || !comp || !input) return 1;

  // --- headset geometry ---
  uint32_t w = 0, h = 0;
  sys->GetRecommendedRenderTargetSize(&w, &h);
  CHECK(w == 1680 && h == 1760, "render target %ux%u", w, h);
  float l, r, t, b;
  sys->GetProjectionRaw(vr::Eye_Left, &l, &r, &t, &b);
  CHECK(near(l, tanf(-0.942f), 1e-4f) && near(r, tanf(0.785f), 1e-4f) && near(t, -tanf(0.873f), 1e-4f) &&
            near(b, tanf(0.873f), 1e-4f),
        "projection raw %f %f %f %f", l, r, t, b);
  vr::HmdMatrix34_t e2h = sys->GetEyeToHeadTransform(vr::Eye_Right);
  CHECK(near(e2h.m[0][3], 0.032f, 1e-5f), "eye to head x %f", e2h.m[0][3]);
  vr::HmdMatrix44_t proj = sys->GetProjectionMatrix(vr::Eye_Left, 0.1f, 100.f);
  CHECK(near(proj.m[3][2], -1, 1e-6f) && proj.m[0][0] > 0, "projection matrix");
  CHECK(near(sys->GetFloatTrackedDeviceProperty(0, vr::Prop_DisplayFrequency_Float, nullptr), 90, 1e-3f), "refresh");

  // --- devices and events ---
  vr::VREvent_t ev;
  int activated = 0;
  vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
  sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
  while (sys->PollNextEvent(&ev, sizeof(ev)))
    if (ev.eventType == vr::VREvent_TrackedDeviceActivated) activated++;
  CHECK(activated == 3, "activated events %d", activated);
  CHECK(sys->GetTrackedDeviceClass(0) == vr::TrackedDeviceClass_HMD &&
            sys->GetTrackedDeviceClass(1) == vr::TrackedDeviceClass_Controller && sys->IsTrackedDeviceConnected(2),
        "device classes");
  CHECK(sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand) == 2, "right role");
  char buf[256];
  sys->GetStringTrackedDeviceProperty(1, vr::Prop_ControllerType_String, buf, sizeof(buf), nullptr);
  CHECK(!strcmp(buf, index ? "knuckles" : "oculus_touch"), "controller type %s", buf);
  vr::ETrackedPropertyError perr;
  uint32_t need = sys->GetStringTrackedDeviceProperty(1, vr::Prop_ModelNumber_String, buf, 2, &perr);
  CHECK(perr == vr::TrackedProp_BufferTooSmall && need > 2, "string property size query");

  // --- input manifest ---
  std::string manifest = data + "/actions.json";
  CHECK(input->SetActionManifestPath(manifest.c_str()) == vr::VRInputError_None, "manifest");
  vr::VRActionSetHandle_t set;
  vr::VRActionHandle_t jump, grab, move, trig, lhand, raim, haptic, skel, menu;
  input->GetActionSetHandle("/actions/main", &set);
  input->GetActionHandle("/actions/main/in/Jump", &jump);
  input->GetActionHandle("/actions/main/in/Grab", &grab);
  input->GetActionHandle("/actions/main/in/Move", &move);
  input->GetActionHandle("/actions/main/in/Trigger", &trig);
  input->GetActionHandle("/actions/main/in/LeftHand", &lhand);
  input->GetActionHandle("/actions/main/in/RightAim", &raim);
  input->GetActionHandle("/actions/main/out/Haptic", &haptic);
  input->GetActionHandle("/actions/main/in/SkeletonLeft", &skel);
  input->GetActionHandle("/actions/main/in/Menu", &menu);
  vr::VRActionHandle_t jump2;
  input->GetActionHandle("/ACTIONS/MAIN/IN/JUMP", &jump2);
  CHECK(jump2 == jump, "handles are case-insensitive");
  vr::VRInputValueHandle_t right_hand, left_hand;
  input->GetInputSourceHandle("/user/hand/right", &right_hand);
  input->GetInputSourceHandle("/user/hand/left", &left_hand);

  // --- frame loop ---
  float prev_yaw = 0;
  uint64_t prev_frame = 0;
  int pressed_seen = 0, released_seen = 0, changed_events = 0, submits_ok = 0, pose_ok = 0, menu_seen = 0;
  bool haptic_sent = false;
  for (int i = 0; i < frames; i++) {
    vr::TrackedDevicePose_t render[vr::k_unMaxTrackedDeviceCount], game[vr::k_unMaxTrackedDeviceCount];
    vr::EVRCompositorError ce = comp->WaitGetPoses(render, vr::k_unMaxTrackedDeviceCount, game, vr::k_unMaxTrackedDeviceCount);
    CHECK(ce == vr::VRCompositorError_None, "WaitGetPoses %d", ce);
    float since;
    uint64_t frame;
    sys->GetTimeSinceLastVsync(&since, &frame);
    CHECK(frame > prev_frame, "frame counter must advance (%llu after %llu)", (unsigned long long)frame,
          (unsigned long long)prev_frame);
    CHECK(render[0].bPoseIsValid && render[1].bPoseIsValid && render[2].bPoseIsValid && !render[3].bDeviceIsConnected,
          "render poses valid");
    float yaw = yaw_of(render[0].mDeviceToAbsoluteTracking);
    if (i > 0) {
      // head yaws at 0.5 rad/s: per frame 0.5/90 rad (more if frames were skipped)
      float d = remainderf(yaw - prev_yaw, 2 * (float)M_PI), expect = 0.5f / 90.f * (float)(frame - prev_frame);
      if (near(d, expect, 0.002f)) pose_ok++;
    }
    CHECK(near(render[0].mDeviceToAbsoluteTracking.m[1][3], 1.7f, 1e-4f), "head height");
    prev_yaw = yaw, prev_frame = frame;

    // prediction: 100 ms ahead = 0.05 rad more yaw
    vr::TrackedDevicePose_t now_p[3], later[3];
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, now_p, 3);
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.1f, later, 3);
    float dy = remainderf(yaw_of(later[0].mDeviceToAbsoluteTracking) - yaw_of(now_p[0].mDeviceToAbsoluteTracking), 2 * (float)M_PI);
    CHECK(near(dy, 0.05f, 0.002f), "extrapolated yaw delta %f", dy);

    // input
    vr::VRActiveActionSet_t as{};
    as.ulActionSet = set;
    CHECK(input->UpdateActionState(&as, sizeof(as), 1) == vr::VRInputError_None, "UpdateActionState");
    vr::InputDigitalActionData_t dj{}, dg{}, dm{};
    vr::InputAnalogActionData_t am{}, at{};
    input->GetDigitalActionData(jump, &dj, sizeof(dj), vr::k_ulInvalidInputValueHandle);
    input->GetDigitalActionData(grab, &dg, sizeof(dg), vr::k_ulInvalidInputValueHandle);
    input->GetDigitalActionData(menu, &dm, sizeof(dm), vr::k_ulInvalidInputValueHandle);
    input->GetAnalogActionData(move, &am, sizeof(am), vr::k_ulInvalidInputValueHandle);
    input->GetAnalogActionData(trig, &at, sizeof(at), vr::k_ulInvalidInputValueHandle);
    CHECK(dj.bActive && dg.bActive && am.bActive && at.bActive, "actions active");
    if (dj.bChanged) changed_events++;
    if (dm.bState) menu_seen++;
    vr::VRControllerState_t cs{};
    bool legacy = sys->GetControllerState(2, &cs, sizeof(cs));
    if (frame >= 45 && frame <= 75) {
      CHECK(dj.bState && dj.activeOrigin == right_hand, "Jump pressed at frame %llu", (unsigned long long)frame);
      CHECK(dg.bState, "Grab (left grip 0.9) held");
      CHECK(near(am.x, 0.5f, 1e-6f) && near(am.y, -0.5f, 1e-6f) && am.activeOrigin == left_hand, "Move %f %f", am.x, am.y);
      CHECK(near(at.x, 0.75f, 1e-6f), "Trigger %f", at.x);
      CHECK(legacy && (cs.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_A)) && near(cs.rAxis[1].x, 0.75f, 1e-6f),
            "legacy right controller state");
      vr::InputDigitalActionData_t dl{};
      input->GetDigitalActionData(jump, &dl, sizeof(dl), left_hand);
      CHECK(!dl.bState, "Jump restricted to the left hand must be off");
      pressed_seen++;
      if (!haptic_sent) {
        CHECK(input->TriggerHapticVibrationAction(haptic, 0, 0.1f, 160, 0.5f, right_hand) == vr::VRInputError_None, "haptic");
        haptic_sent = true;
      }
    } else if (frame >= 100) {
      CHECK(!dj.bState && !dg.bState && am.x == 0 && at.x == 0, "released at frame %llu", (unsigned long long)frame);
      released_seen++;
    }

    vr::InputPoseActionData_t pl{}, pa{};
    input->GetPoseActionDataForNextFrame(lhand, vr::TrackingUniverseStanding, &pl, sizeof(pl), vr::k_ulInvalidInputValueHandle);
    input->GetPoseActionDataRelativeToNow(raim, vr::TrackingUniverseStanding, 0, &pa, sizeof(pa), vr::k_ulInvalidInputValueHandle);
    CHECK(pl.bActive && pl.pose.bPoseIsValid && near(pl.pose.mDeviceToAbsoluteTracking.m[0][3], -0.2f, 1e-4f) &&
              near(pl.pose.mDeviceToAbsoluteTracking.m[2][3], -0.3f, 1e-4f),
          "left grip pose action");
    CHECK(pa.bActive && near(pa.pose.mDeviceToAbsoluteTracking.m[0][3], 0.2f, 1e-4f) &&
              near(pa.pose.mDeviceToAbsoluteTracking.m[2][3], -0.35f, 1e-4f),
          "right aim pose action");

    vr::InputSkeletalActionData_t sk{};
    CHECK(input->GetSkeletalActionData(skel, &sk, sizeof(sk)) == vr::VRInputError_None && sk.bActive, "skeleton action");
    vr::VRSkeletalSummaryData_t sum{};
    input->GetSkeletalSummaryData(skel, vr::VRSummaryType_FromDevice, &sum);
    if (frame >= 45 && frame <= 75) CHECK(near(sum.flFingerCurl[2], 0.9f, 1e-6f), "left middle curl %f", sum.flFingerCurl[2]);

    // submit both eyes as Proton's patched vrclient_x64 does for a DXMT texture
    svr_metal_texture tex[2] = {};
    for (int e = 0; e < 2; e++) {
      tex[e].magic = SVR_METAL_TEXTURE_MAGIC;
      tex[e].size = sizeof(tex[e]);
      tex[e].handle = 0x1000 + e;
      tex[e].version = 1;
      tex[e].width = w, tex[e].height = h, tex[e].format = 81 /* MTLPixelFormatRGBA8Unorm_sRGB */;
      tex[e].array_size = 1, tex[e].sample_count = 1;
      tex[e].event_handle = 0x2000, tex[e].event_value = (uint64_t)i + 1;
    }
    vr::VRTextureBounds_t bounds{0, 0, 1, 1};
    vr::Texture_t vt[2] = {{&tex[0], vr::TextureType_Metal, vr::ColorSpace_Gamma}, {&tex[1], vr::TextureType_Metal, vr::ColorSpace_Gamma}};
    vr::EVRCompositorError s0 = comp->Submit(vr::Eye_Left, &vt[0], &bounds, vr::Submit_Default);
    vr::EVRCompositorError s1 = comp->Submit(vr::Eye_Right, &vt[1], &bounds, vr::Submit_Default);
    if (s0 == vr::VRCompositorError_None && s1 == vr::VRCompositorError_None) submits_ok++;
    comp->PostPresentHandoff();
  }
  // a texture that is not a DXMT description must be refused
  int bogus = 0;
  vr::Texture_t bt{&bogus, vr::TextureType_Metal, vr::ColorSpace_Auto};
  CHECK(comp->Submit(vr::Eye_Left, &bt, nullptr, vr::Submit_Default) == vr::VRCompositorError_InvalidTexture, "bogus texture");
  vr::Texture_t dx{&bogus, vr::TextureType_DirectX, vr::ColorSpace_Auto};
  CHECK(comp->Submit(vr::Eye_Left, &dx, nullptr, vr::Submit_Default) != vr::VRCompositorError_None, "raw D3D11 texture");

  CHECK(submits_ok == frames, "submits ok %d/%d", submits_ok, frames);
  CHECK(pose_ok >= frames * 8 / 10, "head yaw consistent on %d/%d frames", pose_ok, frames - 1);
  CHECK(pressed_seen >= 10 && released_seen >= 10, "input windows seen: pressed %d released %d", pressed_seen, released_seen);
  CHECK(changed_events == 2, "Jump bChanged %d times (press + release)", changed_events);
  CHECK(menu_seen == 0, "Menu never pressed");
  core->Cleanup();
  printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
