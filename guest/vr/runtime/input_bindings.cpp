// SPDX-License-Identifier: Apache-2.0
#include "input_bindings.h"

#include <math.h>

#include "json.h"

namespace svr {
namespace {
std::string lower(std::string s) {
  for (auto &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

std::string dir_of(const std::string &p) {
  size_t i = p.find_last_of('/');
  return i == std::string::npos ? "." : p.substr(0, i);
}

// "/user/hand/left/input/trigger" → hand 0, component "trigger"; false for other devices
bool split_input_path(const std::string &path, const char *kind, int *hand, std::string *rest) {
  std::string p = lower(path);
  std::string l = std::string("/user/hand/left/") + kind + "/", r = std::string("/user/hand/right/") + kind + "/";
  if (!p.compare(0, l.size(), l)) return *hand = 0, *rest = p.substr(l.size()), true;
  if (!p.compare(0, r.size(), r)) return *hand = 1, *rest = p.substr(r.size()), true;
  return false;
}

float param(const Json *params, const char *key, float def) {
  const Json *v = params ? params->get(key) : nullptr;
  if (!v) return def;
  if (v->type == Json::Number) return (float)v->num;
  if (v->type == Json::String) return (float)atof(v->str.c_str());
  return def;
}
}  // namespace

bool Manifest::load_bindings_text(const std::string &text, std::string *err) {
  Json root;
  if (!Json::parse(text, &root, err)) return false;
  const Json *sets = root.get("bindings");
  if (!sets || !sets->is_obj()) return true;  // a binding file without bindings is legal
  for (auto &skv : sets->obj) {
    const std::string set = lower(skv.first);
    const Json &s = skv.second;
    if (const Json *sources = s.get("sources"); sources && sources->is_arr()) {
      for (auto &src : sources->arr) {
        Binding b;
        b.set = set;
        b.input_path = lower(src.get_str("path"));
        b.mode = lower(src.get_str("mode"));
        if (!split_input_path(b.input_path, "input", &b.hand, &b.component)) continue;
        const Json *params = src.get("parameters");
        b.click_threshold = params && params->get("click_activate_threshold");
        b.click_activate = param(params, "click_activate_threshold", b.click_activate);
        b.click_deactivate = param(params, "click_deactivate_threshold", b.click_deactivate);
        if (b.click_deactivate > b.click_activate) b.click_deactivate = b.click_activate;
        b.dpad_touch = params && lower(params->get_str("sub_mode")) == "touch";
        const Json *inputs = src.get("inputs");
        if (!inputs || !inputs->is_obj()) continue;
        for (auto &ikv : inputs->obj) {
          Binding bi = b;
          bi.slot = lower(ikv.first);
          bi.output = ikv.second.get_str("output");
          if (!bi.output.empty()) bindings.push_back(bi);
        }
      }
    }
    if (const Json *cj = s.get("chords"); cj && cj->is_arr()) {
      for (auto &c : cj->arr) {
        ChordBinding cb;
        cb.set = set;
        cb.output = c.get_str("output");
        const Json *inputs = c.get("inputs");
        bool ok = !cb.output.empty() && inputs && inputs->is_arr() && !inputs->arr.empty();
        for (size_t i = 0; ok && i < inputs->arr.size(); i++) {  // each input is [path, slot]
          const Json &in = inputs->arr[i];
          ChordBinding::Input ci;
          ok = in.is_arr() && in.arr.size() == 2 && in.arr[0].type == Json::String && in.arr[1].type == Json::String &&
               split_input_path(in.arr[0].str, "input", &ci.hand, &ci.component);
          if (!ok) break;
          ci.slot = lower(in.arr[1].str);
          cb.inputs.push_back(ci);
        }
        if (ok) chords.push_back(cb);  // a chord with an input this runtime cannot read never fires
      }
    }
    if (const Json *poses_j = s.get("poses"); poses_j && poses_j->is_arr()) {
      for (auto &pj : poses_j->arr) {
        PoseBinding pb;
        std::string rest;
        pb.set = set;
        pb.output = pj.get_str("output");
        if (!split_input_path(pj.get_str("path"), "pose", &pb.hand, &rest)) continue;
        pb.aim = rest == "tip";
        poses.push_back(pb);
      }
    }
    if (const Json *hj = s.get("haptics"); hj && hj->is_arr()) {
      for (auto &h : hj->arr) {
        HandBinding hb;
        std::string rest;
        hb.set = set;
        hb.output = h.get_str("output");
        if (split_input_path(h.get_str("path"), "output", &hb.hand, &rest)) haptics.push_back(hb);
      }
    }
    if (const Json *kj = s.get("skeleton"); kj && kj->is_arr()) {
      for (auto &k : kj->arr) {
        HandBinding hb;
        std::string rest;
        hb.set = set;
        hb.output = k.get_str("output");
        if (split_input_path(k.get_str("path"), "input", &hb.hand, &rest)) skeletons.push_back(hb);
      }
    }
  }
  return true;
}

bool Manifest::load(const std::string &manifest_path, const std::string &controller_type, std::string *err) {
  *this = Manifest{};
  std::string text;
  if (!read_file(manifest_path, &text)) {
    if (err) *err = "cannot read file";
    return false;
  }
  Json root;
  if (!Json::parse(text, &root, err)) return false;
  if (const Json *acts = root.get("actions"); acts && acts->is_arr())
    for (auto &a : acts->arr) actions.push_back({a.get_str("name"), lower(a.get_str("type")), lower(a.get_str("skeleton"))});

  // choose the binding file: exact controller type, then Touch, then Index, then the first one
  const Json *defs = root.get("default_bindings");
  if (!defs || !defs->is_arr() || defs->arr.empty()) return true;
  const Json *pick = nullptr;
  for (const char *want : {controller_type.c_str(), "oculus_touch", "knuckles"}) {
    for (auto &d : defs->arr)
      if (lower(d.get_str("controller_type")) == want) {
        pick = &d;
        break;
      }
    if (pick) break;
  }
  if (!pick) pick = &defs->arr[0];
  std::string url = pick->get_str("binding_url");
  std::string path = !url.empty() && url[0] == '/' ? url : dir_of(manifest_path) + "/" + url;
  std::string btext, berr;
  if (!read_file(path, &btext)) {
    if (err) *err = "cannot read binding file " + path;
    return true;  // the manifest itself is fine; actions stay unbound
  }
  if (!load_bindings_text(btext, &berr)) {
    if (err) *err = "binding file " + path + ": " + berr;
    return true;
  }
  binding_controller = lower(pick->get_str("controller_type"));
  // the headset's own binding (SteamVR loads it beside the controller's): only /user/head sources are used
  for (auto &d : defs->arr) {
    if (lower(d.get_str("controller_type")) != "generic_hmd") continue;
    std::string hurl = d.get_str("binding_url"), htext;
    std::string hpath = !hurl.empty() && hurl[0] == '/' ? hurl : dir_of(manifest_path) + "/" + hurl;
    Json hroot;
    std::string herr;
    if (!read_file(hpath, &htext) || !Json::parse(htext, &hroot, &herr)) break;
    const Json *hsets = hroot.get("bindings");
    if (!hsets || !hsets->is_obj()) break;
    for (auto &skv : hsets->obj) {
      const Json *sources = skv.second.get("sources");
      if (!sources || !sources->is_arr()) continue;
      for (auto &src : sources->arr) {
        std::string path = lower(src.get_str("path"));
        if (path.compare(0, 11, "/user/head/")) continue;
        const Json *inputs = src.get("inputs");
        if (!inputs || !inputs->is_obj()) continue;
        for (auto &ikv : inputs->obj)
          if (const Json *o = ikv.second.get("output"))
            if (o->type == Json::String) head.push_back({lower(skv.first), lower(o->str), path.substr(11)});
      }
    }
    break;
  }
  for (auto &b : bindings) b.output = lower(b.output);
  for (auto &c : chords) c.output = lower(c.output);
  for (auto &p : poses) p.output = lower(p.output);
  for (auto &h : haptics) h.output = lower(h.output);
  for (auto &k : skeletons) k.output = lower(k.output);
  return true;
}

static float raw(const InputSnapshot &in, int hand, const std::string &comp, const std::string &sub) {
  const float *v = in.v[hand];
  if (comp == "trigger") {
    if (sub == "click") return fmaxf(v[SVR_IN_TRIGGER_CLICK], v[SVR_IN_TRIGGER_VALUE] >= 0.95f ? 1.0f : 0.0f);
    if (sub == "touch") return fmaxf(v[SVR_IN_TRIGGER_TOUCH], v[SVR_IN_TRIGGER_VALUE] > 0.05f ? 1.0f : 0.0f);
    return v[SVR_IN_TRIGGER_VALUE];
  }
  if (comp == "grip") {
    if (sub == "click") return fmaxf(v[SVR_IN_SQUEEZE_CLICK], v[SVR_IN_SQUEEZE_VALUE] >= 0.95f ? 1.0f : 0.0f);
    if (sub == "touch") return v[SVR_IN_SQUEEZE_VALUE] > 0.05f ? 1.0f : 0.0f;
    if (sub == "force") return v[SVR_IN_SQUEEZE_FORCE];
    return v[SVR_IN_SQUEEZE_VALUE];
  }
  if (comp == "joystick" || comp == "thumbstick") {
    if (sub == "x") return v[SVR_IN_THUMBSTICK_X];
    if (sub == "y") return v[SVR_IN_THUMBSTICK_Y];
    if (sub == "click") return v[SVR_IN_THUMBSTICK_CLICK];
    if (sub == "touch") return v[SVR_IN_THUMBSTICK_TOUCH];
    return 0;
  }
  if (comp == "trackpad") {
    if (sub == "x") return v[SVR_IN_TRACKPAD_X];
    if (sub == "y") return v[SVR_IN_TRACKPAD_Y];
    if (sub == "touch") return v[SVR_IN_TRACKPAD_TOUCH];
    if (sub == "force") return v[SVR_IN_TRACKPAD_FORCE];
    if (sub == "click") return v[SVR_IN_TRACKPAD_FORCE] > 0.6f ? 1.0f : 0.0f;
    return 0;
  }
  // face buttons: a/x share the lower slot, b/y the upper (left hand reports X/Y)
  if (comp == "a" || comp == "x") return sub == "touch" ? v[SVR_IN_A_TOUCH] : v[SVR_IN_A_CLICK];
  if (comp == "b" || comp == "y") return sub == "touch" ? v[SVR_IN_B_TOUCH] : v[SVR_IN_B_CLICK];
  if (comp == "system") return sub == "touch" ? 0 : v[SVR_IN_SYSTEM_CLICK];
  if (comp == "application_menu" || comp == "menu") return sub == "touch" ? 0 : v[SVR_IN_MENU_CLICK];
  if (comp == "thumbrest") return v[SVR_IN_THUMBREST_TOUCH];
  return 0;
}

void evaluate(const Manifest &m, const std::vector<std::string> &active_sets, const InputSnapshot &in, int64_t now,
              const std::function<void(const std::string &, int, const ActionValue &)> &emit) {
  std::vector<ActiveSet> sets;
  for (auto &s : active_sets) sets.push_back({s, 3});
  if (sets.empty()) {
    for (auto &b : m.bindings) sets.push_back({b.set, 3});
    for (auto &c : m.chords) sets.push_back({c.set, 3});
  }
  evaluate(m, sets, in, now, emit);
}

void evaluate(const Manifest &m, const std::vector<ActiveSet> &active_sets, const InputSnapshot &in, int64_t,
              const std::function<void(const std::string &, int, const ActionValue &)> &emit) {
  if (active_sets.empty()) return;
  auto is_active = [&](const std::string &set, int hand) {
    for (auto &s : active_sets)
      if ((s.hands >> hand & 1) && s.name == set) return true;
    return false;
  };
  for (const Binding &b : m.bindings) {
    if (!is_active(b.set, b.hand)) continue;
    ActionValue out;
    const std::string &c = b.component, &s = b.slot;
    auto digital = [&](float x) {
      out.digital = x > 0.5f;
      out.v[0] = out.digital ? 1.0f : 0.0f;
    };
    if (b.mode == "trigger" && s == "pull") {
      out.v[0] = raw(in, b.hand, c, "value");
      bool press = b.latched ? out.v[0] > b.click_deactivate : out.v[0] >= b.click_activate;
      b.latched = press;
      out.digital = press;
    } else if ((b.mode == "joystick" || b.mode == "trackpad") && s == "position") {
      out.v[0] = raw(in, b.hand, c, "x");
      out.v[1] = raw(in, b.hand, c, "y");
      out.digital = fabsf(out.v[0]) + fabsf(out.v[1]) > 0.5f;
    } else if (b.mode == "scalar_constant" && s == "value") {
      out.v[0] = raw(in, b.hand, c, "value");
      out.digital = out.v[0] > 0.5f;
    } else if (b.mode == "force_sensor" && s == "force") {
      out.v[0] = raw(in, b.hand, c, "force");
      if (out.v[0] == 0 && c == "grip") out.v[0] = raw(in, b.hand, c, "value");
      out.digital = out.v[0] > 0.5f;
    } else if (b.mode == "grab" && (s == "grab" || s == "held")) {
      float v = raw(in, b.hand, c, "value");
      float f = raw(in, b.hand, c, "force");
      float x = fmaxf(v, f);
      b.latched = b.latched ? x > 0.6f : x >= 0.7f;
      out.digital = b.latched;
      out.v[0] = x;
    } else if (b.mode == "toggle_button" && (s == "click" || s == "on")) {
      bool press = raw(in, b.hand, c, "click") > 0.5f;
      if (press && !b.prev_press) b.toggled = !b.toggled;
      b.prev_press = press;
      digital(b.toggled ? 1.0f : 0.0f);
    } else if (b.mode == "dpad") {
      float x = raw(in, b.hand, c, "x"), y = raw(in, b.hand, c, "y");
      bool engaged = raw(in, b.hand, c, b.dpad_touch ? "touch" : "click") > 0.5f;
      const char *dir = "center";
      if (x * x + y * y > 0.25f) {
        float a = atan2f(y, x);  // radians, 0 = east
        dir = fabsf(a) < 0.785f ? "east" : a >= 0.785f && a < 2.356f ? "north" : a <= -0.785f && a > -2.356f ? "south" : "west";
      }
      digital(engaged && s == dir ? 1.0f : 0.0f);
    } else if (s == "click" && b.click_threshold && (c == "trigger" || c == "grip")) {
      // a click the binding puts on the analog travel (Touch has no trigger or grip switch)
      float x = fmaxf(raw(in, b.hand, c, "value"), raw(in, b.hand, c, "force"));
      b.latched = b.latched ? x > b.click_deactivate : x >= b.click_activate;
      digital(b.latched || raw(in, b.hand, c, "click") > 0.5f ? 1.0f : 0.0f);
    } else if (s == "click" || s == "touch") {
      // button mode and the click/touch inputs of trigger/joystick/trackpad/grab modes
      digital(raw(in, b.hand, c, s));
    } else {
      continue;  // slot this runtime does not produce (e.g. "double", "long", "held")
    }
    emit(b.output, b.hand, out);
  }
  for (const ChordBinding &cb : m.chords) {
    bool all = true, set_active = true;
    for (auto &ci : cb.inputs) {
      set_active = set_active && is_active(cb.set, ci.hand);
      // held/single/long/double are press variants of the same switch; the chord needs it down
      all = all && raw(in, ci.hand, ci.component, ci.slot == "touch" ? "touch" : "click") > 0.5f;
    }
    if (!set_active) continue;
    ActionValue out;
    out.digital = all;
    out.v[0] = all ? 1.0f : 0.0f;
    emit(cb.output, cb.inputs[0].hand, out);
  }
}

void finger_curls(int, const std::function<float(int)> &in, float out[5]) {
  bool thumb_down = in(SVR_IN_THUMBSTICK_TOUCH) > 0.5f || in(SVR_IN_A_TOUCH) > 0.5f || in(SVR_IN_B_TOUCH) > 0.5f ||
                    in(SVR_IN_THUMBREST_TOUCH) > 0.5f || in(SVR_IN_TRACKPAD_TOUCH) > 0.5f;
  out[0] = thumb_down ? 1.0f : 0.0f;
  float trig = in(SVR_IN_TRIGGER_VALUE);
  out[1] = trig > 0 ? trig : (in(SVR_IN_TRIGGER_TOUCH) > 0.5f ? 0.35f : 0.0f);
  float grip = fmaxf(in(SVR_IN_SQUEEZE_VALUE), in(SVR_IN_SQUEEZE_FORCE));
  out[2] = out[3] = out[4] = grip;
}
}  // namespace svr
