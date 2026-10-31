// SPDX-License-Identifier: Apache-2.0
// Unit tests for the runtime's pure parts: JSON, binding parsing and evaluation, pose math, the hand skeleton
// and the overlay state.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>

#include "input_bindings.h"
#include "interfaces.h"
#include "json.h"
#include "skeleton.h"
#include "vrmath.h"

using namespace svr;

static int failures, checks;
#define CHECK(c)                                               \
  do {                                                         \
    checks++;                                                  \
    if (!(c)) failures++, printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
  } while (0)

static bool near(float a, float b, float e = 1e-5f) { return fabsf(a - b) <= e; }

static void test_json() {
  Json j;
  std::string err;
  CHECK(Json::parse("{\"a\": [1, 2.5, -3e2], \"b\": \"x\\u00e9\\n\", \"c\": true, \"d\": null,}", &j, &err));
  CHECK(j.get("a")->arr.size() == 3 && near((float)j.get("a")->arr[2].num, -300));
  CHECK(j.get_str("b") == "x\xc3\xa9\n");
  CHECK(j.get("c")->b && j.get("d")->type == Json::Null);
  CHECK(!Json::parse("{\"a\": }", &j, &err) && !err.empty());
  CHECK(!Json::parse("[1] x", &j, &err));
  CHECK(Json::parse("\xef\xbb\xbf// comment\n{\"k\":\"\\ud83d\\ude00\"}", &j, &err) && j.get_str("k") == "\xf0\x9f\x98\x80");
}

static const char *kBindings = R"({"bindings": {"/actions/Main": {
  "sources": [
    {"path": "/user/hand/right/input/a", "mode": "toggle_button", "inputs": {"click": {"output": "/actions/main/in/Toggle"}}},
    {"path": "/user/hand/left/input/grip", "mode": "grab", "inputs": {"grab": {"output": "/actions/main/in/Grab"}}},
    {"path": "/user/hand/left/input/joystick", "mode": "dpad", "parameters": {"sub_mode": "touch"},
     "inputs": {"north": {"output": "/actions/main/in/Up"}, "east": {"output": "/actions/main/in/Right"}}},
    {"path": "/user/hand/right/input/trigger", "mode": "trigger", "parameters": {"click_activate_threshold": 0.6},
     "inputs": {"pull": {"output": "/actions/main/in/Fire"}, "touch": {"output": "/actions/main/in/FireTouch"}}},
    {"path": "/user/head/proximity", "mode": "button", "inputs": {"click": {"output": "/actions/main/in/Ignored"}}}
  ],
  "poses": [{"output": "/actions/main/in/Aim", "path": "/user/hand/right/pose/tip"}],
  "haptics": [{"output": "/actions/main/out/Buzz", "path": "/user/hand/left/output/haptic"}]
}, "/actions/other": {"sources": [
    {"path": "/user/hand/right/input/b", "mode": "button", "inputs": {"click": {"output": "/actions/other/in/B"}}}]}}})";

struct Collected {
  std::map<std::string, ActionValue> v;
};

static Collected run(const Manifest &m, const InputSnapshot &in, std::vector<std::string> sets = {"/actions/main"}) {
  Collected c;
  evaluate(m, sets, in, 0, [&](const std::string &a, int, const ActionValue &v) {
    ActionValue &o = c.v[a];
    o.digital = o.digital || v.digital;
    for (int i = 0; i < 3; i++)
      if (fabsf(v.v[i]) > fabsf(o.v[i])) o.v[i] = v.v[i];
  });
  return c;
}

static void test_bindings() {
  Manifest m;
  std::string err;
  CHECK(m.load_bindings_text(kBindings, &err));
  CHECK(m.bindings.size() == 7);  // head source skipped; dpad and trigger give two each
  CHECK(m.poses.size() == 1 && m.poses[0].aim && m.poses[0].hand == 1);
  CHECK(m.haptics.size() == 1 && m.haptics[0].hand == 0);

  InputSnapshot in;
  // toggle: flips on each rising edge
  in.v[1][SVR_IN_A_CLICK] = 1;
  CHECK(run(m, in).v["/actions/main/in/Toggle"].digital);
  CHECK(run(m, in).v["/actions/main/in/Toggle"].digital);  // still held: no flip
  in.v[1][SVR_IN_A_CLICK] = 0;
  CHECK(run(m, in).v["/actions/main/in/Toggle"].digital);
  in.v[1][SVR_IN_A_CLICK] = 1;
  CHECK(!run(m, in).v["/actions/main/in/Toggle"].digital);

  // grab: 0.7 to engage, holds above 0.6
  in.v[0][SVR_IN_SQUEEZE_VALUE] = 0.65f;
  CHECK(!run(m, in).v["/actions/main/in/Grab"].digital);
  in.v[0][SVR_IN_SQUEEZE_VALUE] = 0.75f;
  CHECK(run(m, in).v["/actions/main/in/Grab"].digital);
  in.v[0][SVR_IN_SQUEEZE_VALUE] = 0.65f;
  CHECK(run(m, in).v["/actions/main/in/Grab"].digital);
  in.v[0][SVR_IN_SQUEEZE_VALUE] = 0.5f;
  CHECK(!run(m, in).v["/actions/main/in/Grab"].digital);

  // dpad with sub_mode touch
  in.v[0][SVR_IN_THUMBSTICK_Y] = 0.9f;
  CHECK(!run(m, in).v["/actions/main/in/Up"].digital);  // not touched
  in.v[0][SVR_IN_THUMBSTICK_TOUCH] = 1;
  Collected c = run(m, in);
  CHECK(c.v["/actions/main/in/Up"].digital && !c.v["/actions/main/in/Right"].digital);

  // trigger: analog pull, click threshold from parameters, hysteresis to 0.85 default deactivate... below
  in.v[1][SVR_IN_TRIGGER_VALUE] = 0.65f;
  c = run(m, in);
  CHECK(near(c.v["/actions/main/in/Fire"].v[0], 0.65f) && c.v["/actions/main/in/Fire"].digital);
  CHECK(c.v["/actions/main/in/FireTouch"].digital);  // touch derived from value when the device has no sensor

  // inactive set is not evaluated
  in.v[1][SVR_IN_B_CLICK] = 1;
  CHECK(!run(m, in).v.count("/actions/other/in/B"));
  CHECK(run(m, in, {"/actions/main", "/actions/other"}).v["/actions/other/in/B"].digital);
}

// A Unity SteamVR plugin title's files (tests/data/vrchat: the shape of VRChat's manifest and Touch bindings):
// clicks placed on the analog travel with string thresholds, the same action on both hands, chords, action
// sets restricted to one hand.
static void test_title_bindings(const std::string &dir) {
  struct Hit {
    bool seen = false, digital = false;
    float v[2] = {};
  };
  typedef std::map<std::string, Hit> Hits;  // "<action>@<hand>"
  auto run_sets = [](const Manifest &m, const InputSnapshot &in, const std::vector<ActiveSet> &sets) {
    Hits c;
    evaluate(m, sets, in, 0, [&](const std::string &a, int hand, const ActionValue &v) {
      Hit &h = c[a + (hand ? "@R" : "@L")];
      h.seen = true, h.digital = h.digital || v.digital;
      for (int i = 0; i < 2; i++)
        if (fabsf(v.v[i]) > fabsf(h.v[i])) h.v[i] = v.v[i];
    });
    return c;
  };
  const std::string G = "/actions/global/in/", O = "/actions/one_hand/in/";
  const std::vector<ActiveSet> global = {{"/actions/global", 3}};

  Manifest m;
  std::string err;
  CHECK(m.load(dir + "/actions.json", "oculus_touch", &err) && err.empty());
  CHECK(m.binding_controller == "oculus_touch" && m.actions.size() == 20);
  CHECK(m.actions[0].name == "/actions/Global/in/Pose");  // names keep the manifest's case
  CHECK(m.poses.size() == 2 && m.haptics.size() == 2 && m.skeletons.size() == 2 && m.chords.size() == 1);
  CHECK(m.chords[0].inputs.size() == 4 && m.chords[0].output == G + "safe_mode");
  for (auto &b : m.bindings) CHECK(b.output.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") == std::string::npos);

  InputSnapshot in;
  Hits c = run_sets(m, in, global);
  // bound and in an active set: reported (so the action is active) with a zero value, per hand
  CHECK(c[G + "interact@L"].seen && c[G + "interact@R"].seen && !c[G + "interact@R"].digital);
  CHECK(c[G + "safe_mode@L"].seen && !c[G + "safe_mode@L"].digital);
  CHECK(!c.count(O + "use@R"));  // its set is not active
  CHECK(run_sets(m, in, {}).empty());  // no active set: nothing

  // Interact: button mode on the trigger, click at 0.7, release below 0.55 (thresholds are JSON strings)
  in.v[1][SVR_IN_TRIGGER_VALUE] = 0.6f;
  CHECK(!run_sets(m, in, global)[G + "interact@R"].digital);
  in.v[1][SVR_IN_TRIGGER_VALUE] = 0.75f;
  c = run_sets(m, in, global);
  CHECK(c[G + "interact@R"].digital && !c[G + "interact@L"].digital);
  CHECK(near(c[G + "trigger_axis@R"].v[0], 0.75f) && c[G + "trigger_axis@L"].v[0] == 0);
  in.v[1][SVR_IN_TRIGGER_VALUE] = 0.6f;
  CHECK(run_sets(m, in, global)[G + "interact@R"].digital);  // held by the hysteresis
  in.v[1][SVR_IN_TRIGGER_VALUE] = 0.5f;
  CHECK(!run_sets(m, in, global)[G + "interact@R"].digital);
  // Grab on Touch: button mode on the grip value, 0.7
  in.v[0][SVR_IN_SQUEEZE_VALUE] = 0.8f;
  c = run_sets(m, in, global);
  CHECK(c[G + "grab@L"].digital && !c[G + "grab@R"].digital && near(c[G + "grip_axis@L"].v[0], 0.8f));
  // a click without thresholds keeps the switch (or full travel)
  in.v[0][SVR_IN_TRIGGER_VALUE] = 0.8f;
  CHECK(!run_sets(m, in, {{"/actions/menu", 3}})["/actions/menu/in/select@L"].digital);
  in.v[0][SVR_IN_TRIGGER_CLICK] = 1;
  CHECK(run_sets(m, in, {{"/actions/menu", 3}})["/actions/menu/in/select@L"].digital);
  in.v[0][SVR_IN_TRIGGER_CLICK] = 0, in.v[0][SVR_IN_TRIGGER_VALUE] = 0;

  // a set restricted to the right hand: only that hand's sources; the other set covers the left
  in.v[0][SVR_IN_TRIGGER_VALUE] = 0.3f, in.v[1][SVR_IN_TRIGGER_VALUE] = 0.3f;
  c = run_sets(m, in, {{"/actions/one_hand", 2}, {"/actions/global", 1}});
  CHECK(c[O + "use@R"].digital && !c.count(O + "use@L"));  // 0.25 threshold
  CHECK(c.count(G + "interact@L") && !c.count(G + "interact@R"));
  CHECK(near(c[O + "aim_axis@R"].v[0], 0.3f));

  // sticks, face buttons (left Y / right B share an action), touch sensors
  in = InputSnapshot{};
  in.v[0][SVR_IN_THUMBSTICK_X] = 0.5f, in.v[0][SVR_IN_THUMBSTICK_Y] = -0.5f, in.v[1][SVR_IN_THUMBSTICK_X] = -1;
  in.v[1][SVR_IN_A_CLICK] = 1, in.v[0][SVR_IN_B_TOUCH] = 1, in.v[1][SVR_IN_THUMBSTICK_CLICK] = 1;
  c = run_sets(m, in, global);
  CHECK(near(c[G + "move@L"].v[0], 0.5f) && near(c[G + "move@L"].v[1], -0.5f) && near(c[G + "rotate@R"].v[0], -1));
  CHECK(c[G + "jump@R"].digital && !c[G + "menu@L"].digital && !c[G + "menu@R"].digital);
  CHECK(c[G + "gesture_activator_thumb@L"].digital && !c[G + "gesture_activator_thumb@R"].digital);
  CHECK(c[G + "stick_click@R"].digital && !c[G + "stick_click@L"].digital);

  // chord: both triggers and both upper face buttons
  in = InputSnapshot{};
  in.v[0][SVR_IN_TRIGGER_VALUE] = in.v[1][SVR_IN_TRIGGER_VALUE] = 1;
  in.v[0][SVR_IN_B_CLICK] = 1;
  CHECK(!run_sets(m, in, global)[G + "safe_mode@L"].digital);
  in.v[1][SVR_IN_B_CLICK] = 1;
  CHECK(run_sets(m, in, global)[G + "safe_mode@L"].digital);
  CHECK(!run_sets(m, in, {{"/actions/global", 1}}).count(G + "safe_mode@L"));  // needs its set on both hands

  // Index: the same manifest picks the knuckles file; Grab is the grab mode there
  Manifest k;
  CHECK(k.load(dir + "/actions.json", "knuckles", &err) && k.binding_controller == "knuckles");
  in = InputSnapshot{};
  in.v[1][SVR_IN_SQUEEZE_FORCE] = 0.8f, in.v[0][SVR_IN_THUMBSTICK_X] = 0.25f;
  c = run_sets(k, in, global);
  CHECK(c[G + "grab@R"].digital && !c[G + "grab@L"].digital && near(c[G + "move@L"].v[0], 0.25f));
  // a controller type without a file of its own falls back to Touch; a listed file that is missing leaves the
  // actions unbound but the manifest loaded
  Manifest f;
  CHECK(f.load(dir + "/actions.json", "some_new_controller", &err) && f.binding_controller == "oculus_touch");
  CHECK(f.load(dir + "/actions.json", "vive_controller", &err) && f.actions.size() == 20 && f.bindings.empty() && !err.empty());
}

static void test_math() {
  svr_pose p{};
  p.orientation[3] = 1;
  p.ang_vel[1] = 1.0f;  // 1 rad/s about +y
  p.lin_vel[0] = 2.0f;
  p.flags = SVR_POSE_VALID | SVR_POSE_VEL;
  svr_pose q = extrapolate(p, 0.5f);
  CHECK(near(q.position[0], 1.0f));
  CHECK(near(q.orientation[1], sinf(0.25f)) && near(q.orientation[3], cosf(0.25f)));
  vr::HmdMatrix34_t m = to_matrix34(q.orientation, q.position);
  CHECK(near(atan2f(m.m[0][2], m.m[2][2]), 0.5f));
  svr_pose back = extrapolate(p, -0.5f);  // into the past: rotates the other way
  CHECK(near(back.position[0], -1.0f) && near(back.orientation[1], -sinf(0.25f)));
  svr_pose still = p;
  still.flags = SVR_POSE_VALID;
  CHECK(near(extrapolate(still, 1.0f).position[0], 0));  // no velocity flag: no motion
  // rotation of a vector: 90 degrees about y maps +x to -z
  Quat r{0, sinf((float)M_PI / 4), 0, cosf((float)M_PI / 4)};
  float v[3] = {1, 0, 0}, o[3];
  qrot(r, v, o);
  CHECK(near(o[0], 0) && near(o[2], -1));
  vr::HmdMatrix44_t pm = projection(-1, 1, -1, 1, 0.1f, 100);
  CHECK(near(pm.m[0][0], 1) && near(pm.m[1][1], 1) && near(pm.m[3][2], -1));
  // GetProjectionRaw -> GetProjectionMatrix with a Quest 3-like asymmetric FOV (up 0.87, down -0.91 rad): the FOV's
  // edges must land on the NDC edges, or the game renders a frustum that is not the one the headset displays
  const float fov[4] = {-0.96f, 0.79f, 0.87f, -0.91f};
  float l, r2, t, b;
  raw_tangents(fov, &l, &r2, &t, &b);
  CHECK(t < 0 && b > 0 && l < 0 && r2 > 0);  // SteamVR's signs: top and left negative
  vr::HmdMatrix44_t qm = projection(l, r2, t, b, 0.1f, 100);
  auto ndc = [&](float x, float y, int row) {  // view-space point at z = -1 (y up, -z forward)
    float v[4] = {x, y, -1, 1}, c[4] = {};
    for (int i = 0; i < 4; i++)
      for (int k = 0; k < 4; k++) c[i] += qm.m[i][k] * v[k];
    return c[row] / c[3];
  };
  CHECK(near(ndc(0, tanf(fov[2]), 1), 1));    // up edge -> top of the image
  CHECK(near(ndc(0, tanf(fov[3]), 1), -1));   // down edge -> bottom
  CHECK(near(ndc(tanf(fov[0]), 0, 0), -1));   // left edge
  CHECK(near(ndc(tanf(fov[1]), 0, 0), 1));    // right edge
  float d = 0;
  for (int i = 0; i < 4; i++) d += qm.m[2][i] * (i == 2 ? -0.1f : i == 3 ? 1.f : 0.f);
  CHECK(near(d / 0.1f, 0));  // D3D depth: near plane -> 0
}

static float dist(const vr::VRBoneTransform_t &a, const vr::VRBoneTransform_t &b) {
  float d = 0;
  for (int i = 0; i < 3; i++) d += (a.position.v[i] - b.position.v[i]) * (a.position.v[i] - b.position.v[i]);
  return sqrtf(d);
}

static void test_skeleton() {
  // hierarchy: 31 bones, one root, parents first, the documented order
  int roots = 0;
  for (int i = 0; i < kHandBoneCount; i++) {
    roots += hand_bone_parent(i) < 0;
    CHECK(hand_bone_parent(i) < i);
    CHECK(hand_bone_name(0, i) && hand_bone_name(1, i));
  }
  CHECK(kHandBoneCount == 31 && roots == 1 && hand_bone_parent(kBoneWrist) == kBoneRoot);
  CHECK(hand_bone_parent(kBoneIndex0) == kBoneWrist && hand_bone_parent(kBoneIndex4) == kBoneIndex3);
  CHECK(hand_bone_parent(kBoneAuxPinky) == kBoneRoot && kBoneAuxThumb == 26 && kBonePinky0 == 21);
  CHECK(!strcmp(hand_bone_name(0, kBoneWrist), "wrist_l") && !strcmp(hand_bone_name(1, kBoneIndex4), "finger_index_r_end"));
  CHECK(!hand_bone_name(0, kHandBoneCount) && hand_bone_parent(99) == -1);

  const float open[5] = {0, 0, 0, 0, 0}, fist[5] = {1, 1, 1, 1, 1}, splay0[5] = {0, 0, 0, 0, 0};
  for (int hand = 0; hand < 2; hand++) {
    vr::VRBoneTransform_t o[kHandBoneCount], f[kHandBoneCount], om[kHandBoneCount], fm[kHandBoneCount];
    hand_pose(hand, open, splay0, o);
    hand_pose(hand, fist, splay0, f);
    hand_to_model_space(o, om);
    hand_to_model_space(f, fm);
    for (int i = 0; i < kHandBoneCount; i++) {
      const vr::HmdQuaternionf_t &q = f[i].orientation;
      CHECK(near(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z, 1, 1e-4f));
      CHECK(near(f[i].position.v[3], 1));
      // bones do not stretch: the parent-space offset has the same length open and closed
      if (i < kBoneAuxThumb) CHECK(near(dist(o[i], o[kBoneRoot]), dist(f[i], f[kBoneRoot]), 1e-5f));
    }
    CHECK(near(o[kBoneRoot].orientation.w, 1) && near(dist(o[kBoneRoot], om[kBoneRoot]), 0));
    CHECK(near(dist(om[kBoneWrist], om[kBoneRoot]), 0.1721f, 1e-3f));  // wrist sits behind the controller origin
    // proximal bone lengths of the model (index 73 mm, middle 91 mm)
    CHECK(near(dist(om[kBoneIndex1], om[kBoneIndex2]), 0.045f, 1e-4f) && near(dist(om[kBoneMiddle0], om[kBoneMiddle1]), 0.091f, 1e-4f));
    for (int d = 0; d < 5; d++) {
      int tip = d == 0 ? kBoneThumb3 : kBoneIndex4 + (d - 1) * 5;
      int knuckle = d == 0 ? kBoneThumb2 : kBoneIndex3 + (d - 1) * 5;
      // closing the hand brings every fingertip towards the wrist; an open finger is nearly straight
      CHECK(dist(fm[tip], fm[kBoneWrist]) < dist(om[tip], om[kBoneWrist]) - 0.02f);
      if (d) CHECK(dist(om[tip], om[kBoneWrist]) > 0.15f && dist(fm[tip], fm[kBoneWrist]) < 0.11f);
      // aux bone = the last knuckle in root space, in both spaces
      CHECK(near(dist(f[kBoneAuxThumb + d], fm[knuckle]), 0) && near(dist(fm[kBoneAuxThumb + d], fm[knuckle]), 0));
      CHECK(near(f[kBoneAuxThumb + d].orientation.w, fm[knuckle].orientation.w));
    }
    // fingers fan out in order: index tip next to middle, pinky furthest from index
    CHECK(dist(om[kBoneIndex4], om[kBoneMiddle4]) < dist(om[kBoneIndex4], om[kBoneRing4]) &&
          dist(om[kBoneIndex4], om[kBoneRing4]) < dist(om[kBoneIndex4], om[kBonePinky4]));
    // curl is monotonic
    float prev = 1e9f;
    for (int step = 0; step <= 10; step++) {
      float c[5] = {0, step / 10.0f, 0, 0, 0};
      vr::VRBoneTransform_t t[kHandBoneCount];
      hand_pose(hand, c, splay0, t);
      hand_to_model_space(t, t);
      float d = dist(t[kBoneIndex4], t[kBoneWrist]);
      CHECK(d < prev);
      prev = d;
      CHECK(near(dist(t[kBoneMiddle4], om[kBoneMiddle4]), 0, 1e-6f));  // other fingers stay
    }
    // splay moves the index tip sideways without changing its distance to its knuckle
    float sp[5] = {0, 1, 0, 0, 0};
    vr::VRBoneTransform_t s[kHandBoneCount];
    hand_pose(hand, open, sp, s);
    hand_to_model_space(s, s);
    CHECK(dist(s[kBoneIndex4], om[kBoneIndex4]) > 0.02f && near(dist(s[kBoneIndex4], s[kBoneIndex1]), dist(om[kBoneIndex4], om[kBoneIndex1]), 1e-5f));

    // motion ranges and reference poses
    float lim[5], full[5];
    hand_limit_curls(vr::VRSkeletalMotionRange_WithController, fist, lim);
    hand_limit_curls(vr::VRSkeletalMotionRange_WithoutController, fist, full);
    CHECK(lim[1] < full[1] && near(full[1], 1) && lim[1] > 0.5f);
    const float wild[5] = {-1, 2, 0.5f, 0, 1};
    hand_limit_curls(vr::VRSkeletalMotionRange_WithoutController, wild, full);
    CHECK(near(full[0], 0) && near(full[1], 1) && near(full[2], 0.5f));
    vr::VRBoneTransform_t ref[kHandBoneCount], grip[kHandBoneCount];
    hand_reference_pose(hand, vr::VRSkeletalReferencePose_Fist, ref);
    CHECK(near(dist(ref[kBoneAuxIndex], f[kBoneAuxIndex]), 0));
    hand_reference_pose(hand, vr::VRSkeletalReferencePose_OpenHand, ref);
    CHECK(near(dist(ref[kBoneAuxIndex], o[kBoneAuxIndex]), 0));
    hand_reference_pose(hand, vr::VRSkeletalReferencePose_GripLimit, grip);
    hand_to_model_space(grip, grip);
    float dg = dist(grip[kBoneIndex4], grip[kBoneWrist]);
    CHECK(dg < dist(om[kBoneIndex4], om[kBoneWrist]) && dg > dist(fm[kBoneIndex4], fm[kBoneWrist]));
  }
  // the right hand is the mirror image of the left (x -> -x in model space)
  vr::VRBoneTransform_t l[kHandBoneCount], r[kHandBoneCount];
  const float some[5] = {0.3f, 0.8f, 0.5f, 0.2f, 1.0f}, sp[5] = {0.5f, -0.5f, 0, 0.3f, 1};
  hand_pose(0, some, sp, l);
  hand_pose(1, some, sp, r);
  hand_to_model_space(l, l);
  hand_to_model_space(r, r);
  for (int i = 0; i < kHandBoneCount; i++)
    CHECK(near(l[i].position.v[0], -r[i].position.v[0], 1e-5f) && near(l[i].position.v[1], r[i].position.v[1], 1e-5f) &&
          near(l[i].position.v[2], r[i].position.v[2], 1e-5f));
}

static void test_overlay() {
  vr::IVROverlay *ov = (vr::IVROverlay *)overlay_iface();
  vr::VROverlayHandle_t h = 0, h2 = 0, thumb = 0, found = 0;
  CHECK(ov->FindOverlay("test.a", &found) == vr::VROverlayError_UnknownOverlay && found == vr::k_ulOverlayHandleInvalid);
  CHECK(ov->CreateOverlay("test.a", "Overlay A", &h) == vr::VROverlayError_None && h != vr::k_ulOverlayHandleInvalid);
  CHECK(ov->CreateOverlay("test.a", "again", &h2) == vr::VROverlayError_KeyInUse && h2 == vr::k_ulOverlayHandleInvalid);
  CHECK(ov->CreateOverlay(std::string(200, 'k').c_str(), "x", &h2) == vr::VROverlayError_KeyTooLong);
  CHECK(ov->CreateOverlay("test.n", std::string(200, 'n').c_str(), &h2) == vr::VROverlayError_NameTooLong);
  CHECK(ov->CreateOverlay(nullptr, "x", &h2) == vr::VROverlayError_InvalidParameter);
  CHECK(ov->FindOverlay("test.a", &found) == vr::VROverlayError_None && found == h);
  char buf[vr::k_unVROverlayMaxKeyLength];
  vr::EVROverlayError e = vr::VROverlayError_RequestFailed;
  CHECK(ov->GetOverlayKey(h, buf, sizeof(buf), &e) == 7 && e == vr::VROverlayError_None && !strcmp(buf, "test.a"));
  CHECK(ov->GetOverlayName(h, buf, 4, &e) == 10 && e == vr::VROverlayError_ArrayTooSmall);
  CHECK(ov->SetOverlayName(h, "Renamed") == vr::VROverlayError_None);
  CHECK(ov->GetOverlayName(h, buf, sizeof(buf), &e) == 8 && !strcmp(buf, "Renamed"));
  CHECK(ov->GetOverlayKey(h + 100, buf, sizeof(buf), &e) == 0 && e == vr::VROverlayError_InvalidHandle);

  // SteamVR's defaults, then every property reads back
  float f = 0, r = 0, g = 0, b = 0;
  bool flag = true;
  uint32_t u = 99;
  CHECK(ov->GetOverlayWidthInMeters(h, &f) == vr::VROverlayError_None && near(f, 1));
  CHECK(ov->GetOverlayAlpha(h, &f) == vr::VROverlayError_None && near(f, 1));
  CHECK(ov->GetOverlayColor(h, &r, &g, &b) == vr::VROverlayError_None && near(r, 1) && near(g, 1) && near(b, 1));
  CHECK(!ov->IsOverlayVisible(h) && ov->GetOverlayFlags(h, &u) == vr::VROverlayError_None && u == 0);
  CHECK(ov->SetOverlayWidthInMeters(h, 2.5f) == vr::VROverlayError_None && ov->GetOverlayWidthInMeters(h, &f) == vr::VROverlayError_None && near(f, 2.5f));
  CHECK(ov->SetOverlayAlpha(h, 0.5f) == vr::VROverlayError_None && ov->GetOverlayAlpha(h, &f) == vr::VROverlayError_None && near(f, 0.5f));
  CHECK(ov->SetOverlayColor(h, 0.1f, 0.2f, 0.3f) == vr::VROverlayError_None && ov->GetOverlayColor(h, &r, &g, &b) == vr::VROverlayError_None && near(g, 0.2f));
  CHECK(ov->SetOverlaySortOrder(h, 7) == vr::VROverlayError_None && ov->GetOverlaySortOrder(h, &u) == vr::VROverlayError_None && u == 7);
  CHECK(ov->SetOverlayCurvature(h, 0.25f) == vr::VROverlayError_None && ov->GetOverlayCurvature(h, &f) == vr::VROverlayError_None && near(f, 0.25f));
  CHECK(ov->SetOverlayTexelAspect(h, 2) == vr::VROverlayError_None && ov->GetOverlayTexelAspect(h, &f) == vr::VROverlayError_None && near(f, 2));
  CHECK(ov->SetOverlayFlag(h, vr::VROverlayFlags_SortWithNonSceneOverlays, true) == vr::VROverlayError_None);
  CHECK(ov->SetOverlayFlag(h, vr::VROverlayFlags_NoDashboardTab, true) == vr::VROverlayError_None);
  CHECK(ov->SetOverlayFlag(h, vr::VROverlayFlags_NoDashboardTab, false) == vr::VROverlayError_None);
  CHECK(ov->GetOverlayFlag(h, vr::VROverlayFlags_SortWithNonSceneOverlays, &flag) == vr::VROverlayError_None && flag);
  CHECK(ov->GetOverlayFlag(h, vr::VROverlayFlags_NoDashboardTab, &flag) == vr::VROverlayError_None && !flag);
  CHECK(ov->GetOverlayFlags(h, &u) == vr::VROverlayError_None && u == vr::VROverlayFlags_SortWithNonSceneOverlays);
  vr::VRTextureBounds_t tb{0.1f, 0.2f, 0.9f, 0.8f}, tb2{};
  CHECK(ov->SetOverlayTextureBounds(h, &tb) == vr::VROverlayError_None && ov->GetOverlayTextureBounds(h, &tb2) == vr::VROverlayError_None && near(tb2.vMax, 0.8f));
  vr::EColorSpace cs = vr::ColorSpace_Auto;
  CHECK(ov->SetOverlayTextureColorSpace(h, vr::ColorSpace_Linear) == vr::VROverlayError_None && ov->GetOverlayTextureColorSpace(h, &cs) == vr::VROverlayError_None && cs == vr::ColorSpace_Linear);
  vr::VROverlayInputMethod im = vr::VROverlayInputMethod_Mouse;
  CHECK(ov->GetOverlayInputMethod(h, &im) == vr::VROverlayError_None && im == vr::VROverlayInputMethod_None);
  vr::HmdVector2_t ms{{640, 480}}, ms2{};
  CHECK(ov->SetOverlayMouseScale(h, &ms) == vr::VROverlayError_None && ov->GetOverlayMouseScale(h, &ms2) == vr::VROverlayError_None && near(ms2.v[1], 480));
  CHECK(ov->SetOverlayAlpha(h + 100, 1) == vr::VROverlayError_InvalidHandle && ov->GetOverlayAlpha(h, nullptr) == vr::VROverlayError_InvalidParameter);

  // transforms: the type follows the last setter, getters of another type refuse
  vr::VROverlayTransformType tt = vr::VROverlayTransform_Invalid;
  vr::ETrackingUniverseOrigin org = vr::TrackingUniverseSeated;
  vr::HmdMatrix34_t m = identity34(), m2{};
  m.m[0][3] = 1, m.m[1][3] = 2, m.m[2][3] = -3;
  CHECK(ov->GetOverlayTransformType(h, &tt) == vr::VROverlayError_None && tt == vr::VROverlayTransform_Absolute);
  CHECK(ov->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &m) == vr::VROverlayError_None);
  CHECK(ov->GetOverlayTransformAbsolute(h, &org, &m2) == vr::VROverlayError_None && org == vr::TrackingUniverseStanding && near(m2.m[2][3], -3));
  vr::TrackedDeviceIndex_t dev = 0;
  CHECK(ov->GetOverlayTransformTrackedDeviceRelative(h, &dev, &m2) == vr::VROverlayError_WrongTransformType);
  CHECK(ov->SetOverlayTransformTrackedDeviceRelative(h, 1, &m) == vr::VROverlayError_None);
  CHECK(ov->SetOverlayTransformTrackedDeviceRelative(h, 1000, &m) == vr::VROverlayError_InvalidTrackedDevice);
  CHECK(ov->GetOverlayTransformTrackedDeviceRelative(h, &dev, &m2) == vr::VROverlayError_None && dev == 1 && near(m2.m[1][3], 2));
  CHECK(ov->GetOverlayTransformType(h, &tt) == vr::VROverlayError_None && tt == vr::VROverlayTransform_TrackedDeviceRelative);
  CHECK(ov->GetOverlayTransformAbsolute(h, &org, &m2) == vr::VROverlayError_WrongTransformType);
  CHECK(ov->SetOverlayTransformTrackedDeviceComponent(h, 2, "tip") == vr::VROverlayError_None);
  CHECK(ov->GetOverlayTransformTrackedDeviceComponent(h, &dev, buf, 2) == vr::VROverlayError_ArrayTooSmall);
  CHECK(ov->GetOverlayTransformTrackedDeviceComponent(h, &dev, buf, sizeof(buf)) == vr::VROverlayError_None && dev == 2 && !strcmp(buf, "tip"));

  // visibility and image: accepted, size remembered, nothing is composited
  CHECK(ov->ShowOverlay(h) == vr::VROverlayError_None && ov->IsOverlayVisible(h));
  CHECK(ov->HideOverlay(h) == vr::VROverlayError_None && !ov->IsOverlayVisible(h));
  CHECK(ov->ShowOverlay(h + 100) == vr::VROverlayError_InvalidHandle && !ov->IsOverlayVisible(h + 100));
  uint32_t pixels[4 * 2] = {}, w = 0, ht = 0;
  CHECK(ov->SetOverlayRaw(h, pixels, 4, 2, 4) == vr::VROverlayError_None);
  CHECK(ov->GetOverlayTextureSize(h, &w, &ht) == vr::VROverlayError_None && w == 4 && ht == 2);
  CHECK(ov->SetOverlayRaw(h, pixels, 4, 2, 3) == vr::VROverlayError_InvalidParameter);
  int fake_texture = 0;
  vr::Texture_t tex{&fake_texture, vr::TextureType_DirectX, vr::ColorSpace_Auto}, none{nullptr, vr::TextureType_DirectX, vr::ColorSpace_Auto};
  CHECK(ov->SetOverlayTexture(h, &tex) == vr::VROverlayError_None && ov->SetOverlayTexture(h, &none) == vr::VROverlayError_InvalidTexture);
  CHECK(ov->SetOverlayFromFile(h, "/nonexistent/overlay.png") == vr::VROverlayError_UnableToLoadFile);
  CHECK(ov->ClearOverlayTexture(h) == vr::VROverlayError_None && ov->GetOverlayTextureSize(h, &w, &ht) == vr::VROverlayError_None && w == 0);
  // coordinates on a 2 m wide quad with a 200x100 image: the right edge centre is 1 m along the quad's x axis
  CHECK(ov->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &m) == vr::VROverlayError_None);
  CHECK(ov->SetOverlayWidthInMeters(h, 2) == vr::VROverlayError_None && ov->SetOverlayTexelAspect(h, 1) == vr::VROverlayError_None);
  CHECK(ov->SetOverlayRaw(h, pixels, 200, 100, 4) == vr::VROverlayError_None);
  CHECK(ov->GetTransformForOverlayCoordinates(h, vr::TrackingUniverseStanding, vr::HmdVector2_t{{200, 50}}, &m2) == vr::VROverlayError_None &&
        near(m2.m[0][3], 2) && near(m2.m[1][3], 2) && near(m2.m[2][3], -3));
  CHECK(ov->GetTransformForOverlayCoordinates(h, vr::TrackingUniverseStanding, vr::HmdVector2_t{{100, 100}}, &m2) == vr::VROverlayError_None &&
        near(m2.m[0][3], 1) && near(m2.m[1][3], 2.5f));

  // no dashboard, no input, no keyboard
  vr::VREvent_t ev;
  CHECK(!ov->PollNextOverlayEvent(h, &ev, sizeof(ev)) && !ov->IsDashboardVisible() && !ov->IsHoverTargetOverlay(h));
  CHECK(ov->ShowKeyboard(vr::k_EGamepadTextInputModeNormal, vr::k_EGamepadTextInputLineModeSingleLine, 0, "d", 16, "", 0) == vr::VROverlayError_RequestFailed);
  CHECK(ov->GetPrimaryDashboardDevice() == vr::k_unTrackedDeviceIndexInvalid && ov->WaitFrameSync(10) == vr::VROverlayError_None);
  CHECK(!strcmp(ov->GetOverlayErrorNameFromEnum(vr::VROverlayError_KeyInUse), "VROverlayError_KeyInUse"));

  // dashboard overlay: main + thumbnail; the thumbnail goes with its overlay and cannot be destroyed alone
  CHECK(ov->CreateDashboardOverlay("test.dash", "Dash", &h2, &thumb) == vr::VROverlayError_None && h2 != thumb && thumb != vr::k_ulOverlayHandleInvalid);
  CHECK(ov->GetOverlayTransformType(h2, &tt) == vr::VROverlayError_None && tt == vr::VROverlayTransform_DashboardTab);
  CHECK(ov->GetOverlayTransformType(thumb, &tt) == vr::VROverlayError_None && tt == vr::VROverlayTransform_DashboardThumb);
  CHECK(ov->ShowOverlay(h2) == vr::VROverlayError_None && !ov->IsOverlayVisible(h2) && !ov->IsActiveDashboardOverlay(h2));
  CHECK(ov->SetDashboardOverlaySceneProcess(h2, 42) == vr::VROverlayError_None && ov->GetDashboardOverlaySceneProcess(h2, &u) == vr::VROverlayError_None && u == 42);
  CHECK(ov->DestroyOverlay(thumb) == vr::VROverlayError_ThumbnailCantBeDestroyed);
  CHECK(ov->DestroyOverlay(h2) == vr::VROverlayError_None && ov->GetOverlayAlpha(thumb, &f) == vr::VROverlayError_InvalidHandle);
  CHECK(ov->DestroyOverlay(h) == vr::VROverlayError_None && ov->DestroyOverlay(h) == vr::VROverlayError_InvalidHandle);
  CHECK(ov->FindOverlay("test.a", &found) == vr::VROverlayError_UnknownOverlay);
  CHECK(ov->CreateOverlay("test.a", "back", &h) == vr::VROverlayError_None && ov->DestroyOverlay(h) == vr::VROverlayError_None);
}

int main(int argc, char **argv) {
  test_title_bindings(argc > 1 ? argv[1] : "tests/data/vrchat");
  test_skeleton();
  test_overlay();
  test_json();
  test_bindings();
  test_math();
  printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
