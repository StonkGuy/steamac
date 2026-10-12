// SPDX-License-Identifier: GPL-3.0-or-later
// WiVRn input ids (InputValue::id = WiVRn device_id) -> the guest's adapter-neutral input table (vrlink.h vrl_input,
// enum svr_input in guest/vr/include/steamac_vr_guest.h). Kept in src/wivrn/ so no WiVRn header leaks out.
#pragma once
#include <cstdint>

namespace vrhost
{
// enum svr_input order (guest/vr/include/steamac_vr_guest.h), the hand-relative index of vrl_input.
namespace svr
{
enum
{
	trigger_value,
	trigger_click,
	trigger_touch,
	squeeze_value,
	squeeze_click,
	squeeze_force,
	thumbstick_x,
	thumbstick_y,
	thumbstick_click,
	thumbstick_touch,
	a_click, // left hand: X
	a_touch,
	b_click, // left hand: Y
	b_touch,
	menu_click, // left: menu; right: system
	system_click,
	thumbrest_touch,
	trackpad_x,
	trackpad_y,
	trackpad_touch,
	trackpad_force,
	count,
};
} // namespace svr

// hand: 0 left, 1 right; index: svr_input id. False for ids the guest table does not carry.
bool wivrn_input_to_vrl(uint32_t wivrn_id, int & hand, int & index);
// The inverse, for adapters that speak the guest table natively and must fill InputValue::id (other adapters).
bool vrl_input_to_wivrn(int hand, int index, uint32_t & wivrn_id);
} // namespace vrhost
