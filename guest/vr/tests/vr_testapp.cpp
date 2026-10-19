// SPDX-License-Identifier: Apache-2.0
// vr-testapp: an OpenVR client that loads a runtime the way Proton's vrclient unix side does
// (dlopen <runtime>/bin/linux64/vrclient.so, VRClientCoreFactory("IVRClientCore_003")) and checks poses,
// frame pacing, submits, SteamVR Input actions, haptics and the legacy controller API end to end.
// Pair it with vr-fakehost and tests/data/script.txt (run_e2e.sh); exits 0 when every check passes.
//
//   vr-testapp RUNTIME_DIR DATA_DIR [--frames N] [--profile touch|index] [--title]
//
// --title is the call sequence of a Unity title on the OpenVR XR plugin + SteamVR plugin (VRChat): the action
// manifest (DATA_DIR/vrchat) comes in VR_Init's startup info and SetActionManifestPath is never called, the
// interface is IVRInput_010, handles are fetched for lower-cased paths and every action is read for every input
// source each frame. No such read may fail: the plugin logs each error.
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

}  // namespace

// outside the anonymous namespace: with no derived class visible in this file, GCC would otherwise
// devirtualise calls on it to __cxa_pure_virtual
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

// IVRInput_010 (OpenVR 1.10.30 - 1.16.8): the current interface without the eye tracking calls
class IVRInput010 {
 public:
  virtual vr::EVRInputError SetActionManifestPath(const char *) = 0;
  virtual vr::EVRInputError GetActionSetHandle(const char *, vr::VRActionSetHandle_t *) = 0;
  virtual vr::EVRInputError GetActionHandle(const char *, vr::VRActionHandle_t *) = 0;
  virtual vr::EVRInputError GetInputSourceHandle(const char *, vr::VRInputValueHandle_t *) = 0;
  virtual vr::EVRInputError UpdateActionState(vr::VRActiveActionSet_t *, uint32_t, uint32_t) = 0;
  virtual vr::EVRInputError GetDigitalActionData(vr::VRActionHandle_t, vr::InputDigitalActionData_t *, uint32_t,
                                                 vr::VRInputValueHandle_t) = 0;
  virtual vr::EVRInputError GetAnalogActionData(vr::VRActionHandle_t, vr::InputAnalogActionData_t *, uint32_t,
                                                vr::VRInputValueHandle_t) = 0;
  virtual vr::EVRInputError GetPoseActionDataRelativeToNow(vr::VRActionHandle_t, vr::ETrackingUniverseOrigin, float,
                                                           vr::InputPoseActionData_t *, uint32_t, vr::VRInputValueHandle_t) = 0;
  virtual vr::EVRInputError GetPoseActionDataForNextFrame(vr::VRActionHandle_t, vr::ETrackingUniverseOrigin,
                                                          vr::InputPoseActionData_t *, uint32_t, vr::VRInputValueHandle_t) = 0;
  virtual vr::EVRInputError GetSkeletalActionData(vr::VRActionHandle_t, vr::InputSkeletalActionData_t *, uint32_t) = 0;
  virtual vr::EVRInputError GetDominantHand(vr::ETrackedControllerRole *) = 0;
  virtual vr::EVRInputError SetDominantHand(vr::ETrackedControllerRole) = 0;
  virtual vr::EVRInputError GetBoneCount(vr::VRActionHandle_t, uint32_t *) = 0;
  virtual vr::EVRInputError GetBoneHierarchy(vr::VRActionHandle_t, vr::BoneIndex_t *, uint32_t) = 0;
  virtual vr::EVRInputError GetBoneName(vr::VRActionHandle_t, vr::BoneIndex_t, char *, uint32_t) = 0;
  virtual vr::EVRInputError GetSkeletalReferenceTransforms(vr::VRActionHandle_t, vr::EVRSkeletalTransformSpace,
                                                           vr::EVRSkeletalReferencePose, vr::VRBoneTransform_t *, uint32_t) = 0;
  virtual vr::EVRInputError GetSkeletalTrackingLevel(vr::VRActionHandle_t, vr::EVRSkeletalTrackingLevel *) = 0;
  virtual vr::EVRInputError GetSkeletalBoneData(vr::VRActionHandle_t, vr::EVRSkeletalTransformSpace, vr::EVRSkeletalMotionRange,
                                                vr::VRBoneTransform_t *, uint32_t) = 0;
  virtual vr::EVRInputError GetSkeletalSummaryData(vr::VRActionHandle_t, vr::EVRSummaryType, vr::VRSkeletalSummaryData_t *) = 0;
  virtual vr::EVRInputError GetSkeletalBoneDataCompressed(vr::VRActionHandle_t, vr::EVRSkeletalMotionRange, void *, uint32_t,
                                                          uint32_t *) = 0;
  virtual vr::EVRInputError DecompressSkeletalBoneData(const void *, uint32_t, vr::EVRSkeletalTransformSpace,
                                                       vr::VRBoneTransform_t *, uint32_t) = 0;
  virtual vr::EVRInputError TriggerHapticVibrationAction(vr::VRActionHandle_t, float, float, float, float,
                                                         vr::VRInputValueHandle_t) = 0;
};

namespace {
float yaw_of(const vr::HmdMatrix34_t &m) { return atan2f(m.m[0][2], m.m[2][2]); }
bool near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

// --- the --title sequence ---
struct TitleAction {
  const char *name;  // as the manifest spells it
  char type;         // b boolean, 1 vector1, 2 vector2, p pose, s skeleton, v vibration, ? not in the manifest
  vr::VRActionHandle_t h;
};
TitleAction title_actions[] = {
    {"/actions/Global/in/Pose", 'p', 0},           {"/actions/Global/in/SkeletonLeftHand", 's', 0},
    {"/actions/Global/in/SkeletonRightHand", 's', 0}, {"/actions/Global/in/HeadsetOnHead", 'b', 0},
    {"/actions/Global/in/Safe_Mode", 'b', 0},      {"/actions/Global/in/Interact", 'b', 0},
    {"/actions/Global/in/Grab", 'b', 0},           {"/actions/Global/in/Move", '2', 0},
    {"/actions/Global/in/Rotate", '2', 0},         {"/actions/Global/in/Jump", 'b', 0},
    {"/actions/Global/in/Menu", 'b', 0},           {"/actions/Global/in/Stick_Click", 'b', 0},
    {"/actions/Global/in/Trigger_Axis", '1', 0},   {"/actions/Global/in/Grip_Axis", '1', 0},
    {"/actions/Global/in/Gesture_Activator_Thumb", 'b', 0}, {"/actions/Global/out/Haptic", 'v', 0},
    {"/actions/One_Hand/in/Use", 'b', 0},          {"/actions/One_Hand/in/Aim_Axis", '1', 0},
    {"/actions/Menu/in/Select", 'b', 0},           {"/actions/Menu/in/Unbound", 'b', 0},
    {"/actions/Global/in/Not_In_The_Manifest", '?', 0},
};
// SteamVR_Input_Sources of the plugin: Any is k_ulInvalidInputValueHandle
const char *const title_sources[] = {"/user/hand/left", "/user/hand/right", "/user/foot/left", "/user/foot/right",
                                     "/user/shoulder/left", "/user/shoulder/right", "/user/waist", "/user/chest",
                                     "/user/head", "/user/gamepad", "/user/camera", "/user/keyboard", "/user/treadmill"};
const int kTitleSources = sizeof(title_sources) / sizeof(*title_sources);

std::string lower(std::string s) {
  for (auto &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

struct Title {
  IVRInput010 *in = nullptr;
  vr::VRActionSetHandle_t global = 0, one_hand = 0, menu = 0;
  vr::VRInputValueHandle_t src[1 + kTitleSources] = {};  // [0] any
  int read_errors = 0, reads = 0;

  vr::VRActionHandle_t action(const char *name) const {
    for (auto &a : title_actions)
      if (!strcmp(a.name, name)) return a.h;
    return 0;
  }
  void handles() {
    CHECK(in->GetActionSetHandle("/actions/global", &global) == vr::VRInputError_None && global, "set handle");
    in->GetActionSetHandle("/actions/one_hand", &one_hand);
    in->GetActionSetHandle("/actions/menu", &menu);
    for (auto &a : title_actions)
      CHECK(in->GetActionHandle(lower(a.name).c_str(), &a.h) == vr::VRInputError_None && a.h != vr::k_ulInvalidActionHandle,
            "action handle %s", a.name);
    for (int i = 0; i < kTitleSources; i++)
      CHECK(in->GetInputSourceHandle(title_sources[i], &src[1 + i]) == vr::VRInputError_None && src[1 + i], "source handle %s",
            title_sources[i]);
    vr::VRActionHandle_t again = 0;
    in->GetActionHandle("/actions/Global/in/Jump", &again);
    CHECK(again == action("/actions/Global/in/Jump"), "handles are case-insensitive");
  }
  bool digital(const char *name, int source, bool *active = nullptr, vr::VRInputValueHandle_t *origin = nullptr) {
    vr::InputDigitalActionData_t d{};
    vr::EVRInputError e = in->GetDigitalActionData(action(name), &d, sizeof(d), src[source]);
    CHECK(e == vr::VRInputError_None, "GetDigitalActionData %s source %d: %d", name, source, e);
    if (active) *active = d.bActive;
    if (origin) *origin = d.activeOrigin;
    return d.bState;
  }
  vr::InputAnalogActionData_t analog(const char *name, int source) {
    vr::InputAnalogActionData_t d{};
    vr::EVRInputError e = in->GetAnalogActionData(action(name), &d, sizeof(d), src[source]);
    CHECK(e == vr::VRInputError_None, "GetAnalogActionData %s source %d: %d", name, source, e);
    return d;
  }
  // what the plugin does each frame: one UpdateActionState, then every action for every source
  void frame() {
    vr::VRActiveActionSet_t as[2] = {};
    as[0].ulActionSet = global;                               // both hands
    as[1].ulActionSet = one_hand, as[1].ulRestrictedToDevice = src[2];  // right hand only
    vr::EVRInputError e = in->UpdateActionState(as, sizeof(as[0]), 2);
    CHECK(e == vr::VRInputError_None, "UpdateActionState %d", e);
    for (auto &a : title_actions)
      for (int s = 0; s <= kTitleSources; s++) {
        vr::InputDigitalActionData_t dd;
        vr::InputAnalogActionData_t ad;
        vr::InputPoseActionData_t pd;
        vr::InputSkeletalActionData_t sd;
        memset(&dd, 0xff, sizeof(dd));
        memset(&ad, 0xff, sizeof(ad));
        e = vr::VRInputError_None;
        bool active = false;
        switch (a.type) {
          case 'b': case '?': e = in->GetDigitalActionData(a.h, &dd, sizeof(dd), src[s]), active = dd.bActive; break;
          case '1': case '2': e = in->GetAnalogActionData(a.h, &ad, sizeof(ad), src[s]), active = ad.bActive; break;
          case 'p': e = in->GetPoseActionDataForNextFrame(a.h, vr::TrackingUniverseStanding, &pd, sizeof(pd), src[s]), active = pd.bActive; break;
          case 's':  // the plugin reads both the skeleton and its pose
            e = s ? vr::VRInputError_None : in->GetSkeletalActionData(a.h, &sd, sizeof(sd));
            if (e == vr::VRInputError_None)
              e = in->GetPoseActionDataForNextFrame(a.h, vr::TrackingUniverseStanding, &pd, sizeof(pd), src[s]), active = pd.bActive;
            break;
          default: break;
        }
        reads++;
        if (e != vr::VRInputError_None) {
          if (!read_errors++) printf("FAIL read of %s for source %d: error %d\n", a.name, s, e);
        }
        // nothing is bound to feet, head, gamepad...; an action outside the manifest is inactive everywhere
        if (s > 2 || a.type == '?') CHECK(!active, "%s must be inactive for source %d", a.name, s);
        if (a.type == '?' && !s) {
          CHECK(!dd.bState && !dd.bChanged && !dd.activeOrigin, "unknown action data must be zeroed");
          CHECK(in->GetAnalogActionData(a.h, &ad, sizeof(ad), 0) == vr::VRInputError_None && ad.x == 0 && !ad.bActive,
                "unknown action read as analog");
        }
      }
  }
};
}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: vr-testapp RUNTIME_DIR DATA_DIR [--frames N] [--profile touch|index]\n");
    return 2;
  }
  std::string runtime = argv[1], data = argv[2];
  int frames = 150;
  bool index = false, title = false, standby = false;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--profile") && i + 1 < argc) index = !strcmp(argv[++i], "index");
    else if (!strcmp(argv[i], "--title")) title = true;
    else if (!strcmp(argv[i], "--standby")) standby = true;
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
  // the startup info as Proton hands it on: the manifest path already a Unix path
  std::string startup = title ? "{\"action_manifest_path\":\"" + data + "/vrchat/actions.json\"}" : "";
  vr::EVRInitError err = core->Init(vr::VRApplication_Scene, startup.c_str());
  CHECK(err == vr::VRInitError_None, "Init: %s", core->GetIDForVRInitError(err));
  if (err) return 1;

  auto *sys = (vr::IVRSystem *)core->GetGenericInterface(vr::IVRSystem_Version, &err);
  auto *comp = (vr::IVRCompositor *)core->GetGenericInterface(vr::IVRCompositor_Version, &err);
  auto *input = (vr::IVRInput *)core->GetGenericInterface(vr::IVRInput_Version, &err);
  CHECK(sys && comp && input, "core interfaces missing");
  // Proton resolves HmdSystemFactory next to VRClientCoreFactory and refuses a runtime without it
  auto hmd_factory = (void *(*)(const char *, int *))dlsym(lib, "HmdSystemFactory");
  CHECK(hmd_factory != nullptr, "no HmdSystemFactory export");
  if (hmd_factory) {
    rc = -1;
    CHECK(hmd_factory(vr::IVRSystem_Version, &rc) == sys && rc == 0, "HmdSystemFactory(%s) rc %d", vr::IVRSystem_Version, rc);
    CHECK(!hmd_factory("IVRSystem_001", &rc) && rc == vr::VRInitError_Init_InterfaceNotFound, "HmdSystemFactory unknown rc %d", rc);
  }
  CHECK(!core->GetGenericInterface("IVRSystem_001", &err) && err == vr::VRInitError_Init_InterfaceNotFound,
        "unknown version must be refused");
  CHECK(core->GetGenericInterface(vr::IVRChaperone_Version, &err) && core->GetGenericInterface(vr::IVRSettings_Version, &err) &&
            core->GetGenericInterface(vr::IVRApplications_Version, &err) &&
            core->GetGenericInterface(vr::IVRRenderModels_Version, &err),
        "VRChat's auxiliary interfaces missing");
  if (!sys || !comp || !input) return 1;

  if (standby) {
    // --standby (fake host script-standby.txt: the headset leaves at frame 40, returns at 80). As SteamVR: the HMD
    // stays connected (standby) with an invalid pose, WaitGetPoses keeps pacing, the projection stays, the
    // controllers deactivate and come back; the game never stalls.
    int invalid = 0, deact = 0, act = 0, waits_ok = 0;
    bool hmd_always = true, proj_always = true;
    for (int i = 0; i < 130; i++) {
      vr::TrackedDevicePose_t render[vr::k_unMaxTrackedDeviceCount];
      if (comp->WaitGetPoses(render, vr::k_unMaxTrackedDeviceCount, nullptr, 0) == vr::VRCompositorError_None) waits_ok++;
      if (!render[0].bPoseIsValid) invalid++;
      hmd_always &= sys->IsTrackedDeviceConnected(0) && render[0].bDeviceIsConnected;
      float l, r, t, b;
      sys->GetProjectionRaw(vr::Eye_Left, &l, &r, &t, &b);
      proj_always &= r - l > 1;
      vr::VREvent_t ev;
      while (sys->PollNextEvent(&ev, sizeof(ev))) {
        if (ev.eventType == vr::VREvent_TrackedDeviceDeactivated) {
          CHECK(ev.trackedDeviceIndex != 0, "HMD deactivated");
          deact++;
        }
        if (ev.eventType == vr::VREvent_TrackedDeviceActivated && ev.trackedDeviceIndex != 0) act++;
      }
    }
    printf("standby: waits %d, invalid head poses %d, controller deactivations %d / activations %d\n", waits_ok, invalid,
           deact, act);
    CHECK(waits_ok == 130 && hmd_always && proj_always, "standby: waits %d hmd %d projection %d", waits_ok, hmd_always,
          proj_always);
    CHECK(invalid >= 30 && invalid <= 45 && deact == 2 && act >= 4, "standby: invalid %d deact %d act %d", invalid, deact, act);
    core->Cleanup();
    return failures ? 1 : 0;
  }

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
  {  // GetFrameTimings with an older (smaller) struct: stride is the caller's m_nSize (IVRCompositor_021: 176 bytes)
    alignas(8) unsigned char buf[176 * 2 + 16];
    memset(buf, 0xAB, sizeof(buf));
    uint32_t sz = 176;
    memcpy(buf, &sz, 4);
    CHECK(comp->GetFrameTimings((vr::Compositor_FrameTiming *)buf, 2) == 2, "GetFrameTimings old size");
    uint32_t sz1;
    memcpy(&sz1, buf + 176, 4);
    CHECK(sz1 == 176 && buf[176 * 2] == 0xAB, "GetFrameTimings stride %u", sz1);
  }
  CHECK(sys->GetTrackedDeviceClass(0) == vr::TrackedDeviceClass_HMD &&
            sys->GetTrackedDeviceClass(1) == vr::TrackedDeviceClass_Controller && sys->IsTrackedDeviceConnected(2),
        "device classes");
  CHECK(sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand) == 2, "right role");
  char buf[256];
  sys->GetStringTrackedDeviceProperty(1, vr::Prop_ControllerType_String, buf, sizeof(buf), nullptr);
  CHECK(!strcmp(buf, index ? "knuckles" : "oculus_touch"), "controller type %s", buf);
  // the HMD has a controller type and the family's tracking system, as SteamVR reports them (VRChat ignores an HMD
  // without Prop_ControllerType_String: no head tracking at all)
  buf[0] = 0;
  vr::ETrackedPropertyError herr = vr::TrackedProp_UnknownProperty;
  sys->GetStringTrackedDeviceProperty(0, vr::Prop_ControllerType_String, buf, sizeof(buf), &herr);
  CHECK(herr == vr::TrackedProp_Success && !strcmp(buf, index ? "indexhmd" : "oculus"), "HMD controller type '%s' (%d)", buf, herr);
  sys->GetStringTrackedDeviceProperty(0, vr::Prop_TrackingSystemName_String, buf, sizeof(buf), &herr);
  CHECK(herr == vr::TrackedProp_Success && !strcmp(buf, index ? "lighthouse" : "oculus"), "HMD tracking system '%s'", buf);
  vr::ETrackedPropertyError perr;
  uint32_t need = sys->GetStringTrackedDeviceProperty(1, vr::Prop_ModelNumber_String, buf, 2, &perr);
  CHECK(perr == vr::TrackedProp_BufferTooSmall && need > 2, "string property size query");

  // --- input manifest ---
  Title tt;
  if (title) {
    tt.in = (IVRInput010 *)core->GetGenericInterface("IVRInput_010", &err);
    CHECK(tt.in != nullptr, "IVRInput_010");
    if (!tt.in) return 1;
    tt.handles();
  }
  // handles exist for any path, before and without a manifest; reading one that names no action is not an error
  vr::VRActionHandle_t early = 0;
  vr::InputDigitalActionData_t ed;
  vr::InputAnalogActionData_t ea;
  vr::InputPoseActionData_t ep;
  memset(&ed, 0xff, sizeof(ed));
  CHECK(input->GetActionHandle("/actions/main/in/Jump", &early) == vr::VRInputError_None && early, "handle before the manifest");
  CHECK(input->GetDigitalActionData(early, &ed, sizeof(ed), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None &&
            !ed.bActive && !ed.bState,
        "action read before the manifest: no error, inactive");
  CHECK(input->GetAnalogActionData(early, &ea, sizeof(ea), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None && !ea.bActive,
        "analog read before the manifest");
  CHECK(input->GetPoseActionDataForNextFrame(early, vr::TrackingUniverseStanding, &ep, sizeof(ep), 0) == vr::VRInputError_None &&
            !ep.bActive,
        "pose read before the manifest");
  CHECK(input->TriggerHapticVibrationAction(early, 0, 0.1f, 100, 1, 0) == vr::VRInputError_None, "haptic before the manifest");
  CHECK(input->GetDigitalActionData(vr::k_ulInvalidActionHandle, &ed, sizeof(ed), 0) == vr::VRInputError_InvalidHandle &&
            input->GetDigitalActionData(0x7fffffffffull, &ed, sizeof(ed), 0) == vr::VRInputError_InvalidHandle,
        "the invalid handle and a handle never given out are refused");
  std::string manifest = data + "/actions.json";
  if (!title) CHECK(input->SetActionManifestPath(manifest.c_str()) == vr::VRInputError_None, "manifest");
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
  int skipped = 0;
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
    if (i > 0 && frame != prev_frame + 1) skipped++;
    prev_yaw = yaw, prev_frame = frame;

    // prediction: 100 ms ahead = 0.05 rad more yaw
    vr::TrackedDevicePose_t now_p[3], later[3];
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, now_p, 3);
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.1f, later, 3);
    float dy = remainderf(yaw_of(later[0].mDeviceToAbsoluteTracking) - yaw_of(now_p[0].mDeviceToAbsoluteTracking), 2 * (float)M_PI);
    CHECK(near(dy, 0.05f, 0.002f), "extrapolated yaw delta %f", dy);

    vr::VRControllerState_t cs{};
    bool legacy = sys->GetControllerState(2, &cs, sizeof(cs));
    if (title) {
      tt.frame();
      bool la, ra;
      vr::VRInputValueHandle_t jo = 0;
      bool jump_any = tt.digital("/actions/Global/in/Jump", 0, nullptr, &jo);
      vr::InputDigitalActionData_t dj{};
      tt.in->GetDigitalActionData(tt.action("/actions/Global/in/Jump"), &dj, sizeof(dj), 0);
      if (dj.bChanged) changed_events++;
      if (tt.digital("/actions/Global/in/Menu", 0)) menu_seen++;
      bool il = tt.digital("/actions/Global/in/Interact", 1, &la), ir = tt.digital("/actions/Global/in/Interact", 2, &ra);
      CHECK(la && ra, "Interact is bound on both hands: active for each");
      // the generic_hmd binding: /user/head/proximity drives HeadsetOnHead while the headset is connected
      // (VRChat treats an inactive/false HeadsetOnHead as the headset taken off)
      bool hoa = false;
      bool hon = tt.digital("/actions/Global/in/HeadsetOnHead", 0, &hoa);
      CHECK(hoa && hon, "HeadsetOnHead active and on (active %d, state %d)", hoa, hon);
      bool ua, ula;
      bool use_r = tt.digital("/actions/One_Hand/in/Use", 2, &ua), use_l = tt.digital("/actions/One_Hand/in/Use", 1, &ula);
      CHECK(ua && !ula && !use_l, "One_Hand is active for the right hand only");
      bool sa, na;
      tt.digital("/actions/Menu/in/Select", 0, &sa), tt.digital("/actions/Menu/in/Unbound", 0, &na);
      CHECK(!sa && !na, "an action of a set that is not active, and an unbound one, are inactive");
      vr::InputAnalogActionData_t mv = tt.analog("/actions/Global/in/Move", 0), mvr = tt.analog("/actions/Global/in/Move", 2);
      vr::InputAnalogActionData_t tl = tt.analog("/actions/Global/in/Trigger_Axis", 1), tr = tt.analog("/actions/Global/in/Trigger_Axis", 2);
      vr::InputAnalogActionData_t ta = tt.analog("/actions/Global/in/Trigger_Axis", 0);
      bool grab_l = tt.digital("/actions/Global/in/Grab", 1), grab_r = tt.digital("/actions/Global/in/Grab", 2);
      CHECK(mv.bActive && !mvr.bActive && tl.bActive && tr.bActive, "analog actions active per bound hand");
      if (frame >= 45 && frame <= 75) {
        CHECK(jump_any && jo == tt.src[2] && tt.digital("/actions/Global/in/Jump", 2) && !tt.digital("/actions/Global/in/Jump", 1),
              "Jump (right A) at frame %llu", (unsigned long long)frame);
        CHECK(ir && !il && tt.digital("/actions/Global/in/Interact", 0), "Interact: right trigger 0.75 past the 0.7 click");
        CHECK(use_r, "Use: right trigger past 0.25");
        CHECK(near(tr.x, 0.75f, 1e-6f) && tl.x == 0 && near(ta.x, 0.75f, 1e-6f) && ta.activeOrigin == tt.src[2], "Trigger_Axis %f %f", tl.x, tr.x);
        CHECK(near(mv.x, 0.5f, 1e-6f) && near(mv.y, -0.5f, 1e-6f) && mv.activeOrigin == tt.src[1], "Move %f %f", mv.x, mv.y);
        CHECK(grab_l && !grab_r, "Grab: left grip 0.9");
        CHECK(tt.digital("/actions/Global/in/Gesture_Activator_Thumb", 1) && !tt.digital("/actions/Global/in/Gesture_Activator_Thumb", 2),
              "thumb touch: left stick touched, right thumb on a button that is not bound to it");
        CHECK(!tt.digital("/actions/Global/in/Safe_Mode", 0), "chord not held");
        pressed_seen++;
        if (!haptic_sent) {
          CHECK(tt.in->TriggerHapticVibrationAction(tt.action("/actions/Global/out/Haptic"), 0, 0.1f, 160, 0.5f, tt.src[2]) ==
                    vr::VRInputError_None,
                "haptic");
          CHECK(tt.in->TriggerHapticVibrationAction(tt.action("/actions/Global/out/Haptic"), 0, 0.1f, 160, 0.5f, tt.src[9]) ==
                    vr::VRInputError_None,
                "haptic restricted to the head goes nowhere");
          haptic_sent = true;
        }
      } else if (frame >= 100) {
        CHECK(!jump_any && !il && !ir && !use_r && !grab_l && mv.x == 0 && tr.x == 0, "released at frame %llu", (unsigned long long)frame);
        released_seen++;
      }
      vr::InputPoseActionData_t pl{}, pr{}, ph{};
      vr::VRActionHandle_t pose = tt.action("/actions/Global/in/Pose");
      tt.in->GetPoseActionDataForNextFrame(pose, vr::TrackingUniverseStanding, &pl, sizeof(pl), tt.src[1]);
      tt.in->GetPoseActionDataRelativeToNow(pose, vr::TrackingUniverseStanding, 0, &pr, sizeof(pr), tt.src[2]);
      CHECK(tt.in->GetPoseActionDataForNextFrame(pose, vr::TrackingUniverseStanding, &ph, sizeof(ph), tt.src[9]) == vr::VRInputError_None &&
                !ph.bActive,
            "pose for the head source: inactive");
      {  // reading a digital action as a pose stays a type error
        vr::InputPoseActionData_t pw{};
        CHECK(tt.in->GetPoseActionDataForNextFrame(tt.action("/actions/Global/in/Jump"), vr::TrackingUniverseStanding, &pw, sizeof(pw), 0) ==
                  vr::VRInputError_WrongType,
              "pose read of a boolean action");
      }
      CHECK(pl.bActive && pl.pose.bPoseIsValid && near(pl.pose.mDeviceToAbsoluteTracking.m[0][3], -0.2f, 1e-4f) && pl.activeOrigin == tt.src[1],
            "left hand pose");
      CHECK(pr.bActive && pr.pose.bPoseIsValid && near(pr.pose.mDeviceToAbsoluteTracking.m[0][3], 0.2f, 1e-4f) && pr.activeOrigin == tt.src[2],
            "right hand pose");
      for (int hnd = 0; hnd < 2; hnd++) {
        vr::VRActionHandle_t sk = tt.action(hnd ? "/actions/Global/in/SkeletonRightHand" : "/actions/Global/in/SkeletonLeftHand");
        vr::InputSkeletalActionData_t sd{};
        vr::VRBoneTransform_t bones[31];
        CHECK(tt.in->GetSkeletalActionData(sk, &sd, sizeof(sd)) == vr::VRInputError_None && sd.bActive && sd.activeOrigin == tt.src[1 + hnd],
              "skeleton action %d", hnd);
        CHECK(tt.in->GetSkeletalBoneData(sk, vr::VRSkeletalTransformSpace_Parent, vr::VRSkeletalMotionRange_WithoutController, bones, 31) ==
                  vr::VRInputError_None,
              "skeleton bones %d", hnd);
        // the plugin reads a skeleton action's pose too: the hand's pose, for Any and for its own hand
        vr::InputPoseActionData_t sp{}, spo{}, sph{};
        CHECK(tt.in->GetPoseActionDataForNextFrame(sk, vr::TrackingUniverseStanding, &sp, sizeof(sp), 0) == vr::VRInputError_None &&
                  sp.bActive && sp.pose.bPoseIsValid && sp.activeOrigin == tt.src[1 + hnd] &&
                  near(sp.pose.mDeviceToAbsoluteTracking.m[0][3], hnd ? 0.2f : -0.2f, 1e-4f),
              "pose of skeleton action %d", hnd);
        CHECK(tt.in->GetPoseActionDataRelativeToNow(sk, vr::TrackingUniverseStanding, 0, &spo, sizeof(spo), tt.src[2 - hnd]) ==
                      vr::VRInputError_None &&
                  !spo.bActive,
              "pose of skeleton action %d restricted to the other hand: inactive", hnd);
        CHECK(tt.in->GetPoseActionDataRelativeToNow(sk, vr::TrackingUniverseStanding, 0, &sph, sizeof(sph), tt.src[1 + hnd]) ==
                      vr::VRInputError_None &&
                  sph.bActive,
              "pose of skeleton action %d restricted to its hand", hnd);
      }
    } else {
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
    }  // !title

    // submit both eyes as Proton's patched vrclient_x64 does for a DXMT texture
    svr_metal_texture tex[2] = {};
    for (int e = 0; e < 2; e++) {
      tex[e].magic = SVR_METAL_TEXTURE_MAGIC;
      tex[e].size = sizeof(tex[e]);
      tex[e].handle = 0x1000 + e;
      tex[e].width = w, tex[e].height = h, tex[e].dxgi_format = 28 /* R8G8B8A8_UNORM */;
      tex[e].array_size = 1, tex[e].sample_count = 1;
      tex[e].event_handle = 0x2000, tex[e].event_value = (uint64_t)i * 2 + e + 1;
    }
    // left eye full and upright, right eye a flipped sub-rect (vMin > vMax), as some engines submit
    vr::VRTextureBounds_t bounds{0, 0, 1, 1}, rbounds{0, 1, 0.5f, 0};
    vr::Texture_t vt[2] = {{&tex[0], vr::TextureType_Metal, vr::ColorSpace_Gamma}, {&tex[1], vr::TextureType_Metal, vr::ColorSpace_Gamma}};
    vr::EVRCompositorError s0 = comp->Submit(vr::Eye_Left, &vt[0], &bounds, vr::Submit_Default);
    vr::EVRCompositorError s1 = comp->Submit(vr::Eye_Right, &vt[1], &rbounds, vr::Submit_Default);
    if (s0 == vr::VRCompositorError_None && s1 == vr::VRCompositorError_None) submits_ok++;
    comp->PostPresentHandoff();
  }
  // a texture that is not a DXMT description must be refused
  int bogus = 0;
  vr::Texture_t bt{&bogus, vr::TextureType_Metal, vr::ColorSpace_Auto};
  CHECK(comp->Submit(vr::Eye_Left, &bt, nullptr, vr::Submit_Default) == vr::VRCompositorError_InvalidTexture, "bogus texture");
  vr::Texture_t dx{&bogus, vr::TextureType_DirectX, vr::ColorSpace_Auto};
  CHECK(comp->Submit(vr::Eye_Left, &dx, nullptr, vr::Submit_Default) != vr::VRCompositorError_None, "raw D3D11 texture");

  CHECK(skipped <= frames / 10, "frames skipped %d of %d (pacing)", skipped, frames);
  CHECK(submits_ok == frames, "submits ok %d/%d", submits_ok, frames);
  CHECK(pose_ok >= frames * 8 / 10, "head yaw consistent on %d/%d frames", pose_ok, frames - 1);
  CHECK(pressed_seen >= 10 && released_seen >= 10, "input windows seen: pressed %d released %d", pressed_seen, released_seen);
  CHECK(changed_events == 2, "Jump bChanged %d times (press + release)", changed_events);
  CHECK(menu_seen == 0, "Menu never pressed");
  if (title) {
    CHECK(tt.read_errors == 0, "%d of %d per-frame action reads returned an error", tt.read_errors, tt.reads);
    printf("title: %d action reads, %d errors\n", tt.reads, tt.read_errors);
  }
  core->Cleanup();
  printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
