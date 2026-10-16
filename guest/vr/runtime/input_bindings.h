// SPDX-License-Identifier: Apache-2.0
// SteamVR Input manifest + binding file model and evaluation, independent of the OpenVR interface (unit-tested).
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "steamac_vr_guest.h"

namespace svr {
struct Action {
  std::string name;      // "/actions/main/in/Jump"
  std::string type;      // boolean, vector1, vector2, vector3, pose, skeleton, vibration
  std::string skeleton;  // skeleton actions: "/skeleton/hand/left"
};

// one input of a binding source: e.g. mode "trigger", input "pull" → action
struct Binding {
  std::string set;         // "/actions/main"
  std::string output;      // action name
  int hand = 0;            // 0 left, 1 right
  std::string input_path;  // "/user/hand/left/input/trigger"
  std::string component;   // "trigger", "grip", "joystick", "a", ...
  std::string mode;        // button, trigger, joystick, trackpad, dpad, scalar_constant, force_sensor, grab, toggle_button
  std::string slot;        // click, touch, pull, position, value, force, grab, north, ...
  float click_activate = 0.9f, click_deactivate = 0.85f;  // analog → click thresholds where the mode needs them
  bool click_threshold = false;                           // the source sets click_activate_threshold itself
  bool dpad_touch = false;                                // dpad sub_mode touch instead of click
  // evaluation state (toggle_button, grab and click hysteresis)
  mutable bool latched = false, toggled = false, prev_press = false;
};

struct PoseBinding {
  std::string set, output;
  int hand = 0;
  bool aim = false;  // pose/tip → aim; raw/base/handgrip → grip
};

struct HandBinding {  // haptics and skeleton
  std::string set, output;
  int hand = 0;
};

// all inputs held at once → a boolean action ("chords" of a binding file)
struct ChordBinding {
  struct Input {
    int hand = 0;
    std::string component, slot;
  };
  std::string set, output;
  std::vector<Input> inputs;
};

struct Manifest {
  std::vector<Action> actions;
  std::vector<Binding> bindings;
  std::vector<ChordBinding> chords;
  std::vector<PoseBinding> poses;
  std::vector<HandBinding> haptics;
  std::vector<HandBinding> skeletons;
  // HMD bindings from the manifest's generic_hmd default binding (/user/head/proximity -> e.g. VRChat's
  // HeadsetOnHead): set, output (both lower-case) and the head component
  struct HeadBinding {
    std::string set, output, component;
  };
  std::vector<HeadBinding> head;
  std::string binding_controller;  // controller_type of the binding file that was loaded ("" = none)

  // Load the manifest and the default binding file for `controller_type` (falls back to oculus_touch,
  // then knuckles, then the first one listed). Returns false only for an unreadable/invalid manifest.
  // Binding outputs and set names come out lower-case (SteamVR Input paths are case-insensitive).
  bool load(const std::string &manifest_path, const std::string &controller_type, std::string *err);
  // parse a binding file's JSON text (exposed for tests)
  bool load_bindings_text(const std::string &text, std::string *err);
};

struct InputSnapshot {
  float v[2][SVR_IN_COUNT] = {};
};

struct ActionValue {
  bool digital = false;
  float v[3] = {};
};

// an action set as UpdateActionState activates it: for both hands or restricted to one
struct ActiveSet {
  std::string name;    // lower-case
  unsigned hands = 3;  // bit 0 left, bit 1 right
};

// Evaluate all bindings of the active action sets (empty = all sets) against the snapshot and call
// emit(action, hand, value) for each bound source. Toggle and click-hysteresis state persist per manifest.
void evaluate(const Manifest &m, const std::vector<std::string> &active_sets, const InputSnapshot &in, int64_t now,
              const std::function<void(const std::string &, int, const ActionValue &)> &emit);
// The same for sets with a device restriction; here an empty list means no set is active.
void evaluate(const Manifest &m, const std::vector<ActiveSet> &active_sets, const InputSnapshot &in, int64_t now,
              const std::function<void(const std::string &, int, const ActionValue &)> &emit);

// finger curls (thumb, index, middle, ring, pinky) from controller inputs
void finger_curls(int hand, const std::function<float(int)> &input, float out[5]);
}  // namespace svr
