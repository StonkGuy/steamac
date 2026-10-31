// SPDX-License-Identifier: Apache-2.0
// Unit tests for the runtime's pure parts: JSON, binding parsing and evaluation, pose math.
#include <math.h>
#include <stdio.h>

#include <map>
#include <string>

#include "input_bindings.h"
#include "json.h"
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
}

int main() {
  test_json();
  test_bindings();
  test_math();
  printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
