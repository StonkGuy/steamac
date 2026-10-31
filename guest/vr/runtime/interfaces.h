// SPDX-License-Identifier: Apache-2.0
// The interface objects GetGenericInterface hands out (one instance each, process lifetime).
#pragma once
#include <string>

#include "openvr.h"

namespace svr {
void *system_iface();
void *compositor_iface();
void *input_iface();
void *chaperone_iface();
void *settings_iface();
void *applications_iface();
void *render_models_iface();
void *overlay_iface();
void *screenshots_iface();
void *overlay_view_iface();

// device description used by IVRSystem properties and IVRInput origins
struct ControllerInfo {
  const char *controller_type;  // binding file key: "oculus_touch", "knuckles"
  const char *model;
  const char *render_model;
  const char *tracking_system;
  const char *manufacturer;
  const char *input_profile;
};
ControllerInfo controller_info(int hand);  // hand 0 left, 1 right, for the headset's current profile

// legacy (IVRSystem::GetControllerState) state from the input snapshot
bool legacy_controller_state(int hand, vr::VRControllerState_t *state);

// for tests: reset IVRInput state between runs in one process
void input_reset();
void input_startup_info(const char *startup_json);
}  // namespace svr
