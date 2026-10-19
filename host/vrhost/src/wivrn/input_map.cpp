// SPDX-License-Identifier: GPL-3.0-or-later
#include "input_map.h"
#include "vrlink.h"
#include "wivrn_packets.h"

namespace vrhost
{
namespace
{
// enum svr_input order (guest/vr/include/steamac_vr_guest.h); checked against VRL_INPUT_COUNT below
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
static_assert(count == VRL_INPUT_COUNT, "input table out of sync with vrlink.h / steamac_vr_guest.h");
} // namespace

bool wivrn_input_to_vrl(uint32_t wivrn_id, int & hand, int & index)
{
	using d = wivrn::device_id;
	struct entry
	{
		d id;
		int hand, index;
	};
	static constexpr entry table[] = {
	        {d::LEFT_TRIGGER_VALUE, 0, trigger_value},     {d::RIGHT_TRIGGER_VALUE, 1, trigger_value},
	        {d::LEFT_TRIGGER_CLICK, 0, trigger_click},     {d::RIGHT_TRIGGER_CLICK, 1, trigger_click},
	        {d::LEFT_TRIGGER_TOUCH, 0, trigger_touch},     {d::RIGHT_TRIGGER_TOUCH, 1, trigger_touch},
	        {d::LEFT_SQUEEZE_VALUE, 0, squeeze_value},     {d::RIGHT_SQUEEZE_VALUE, 1, squeeze_value},
	        {d::LEFT_SQUEEZE_CLICK, 0, squeeze_click},     {d::RIGHT_SQUEEZE_CLICK, 1, squeeze_click},
	        {d::LEFT_SQUEEZE_FORCE, 0, squeeze_force},     {d::RIGHT_SQUEEZE_FORCE, 1, squeeze_force},
	        {d::LEFT_THUMBSTICK_X, 0, thumbstick_x},       {d::RIGHT_THUMBSTICK_X, 1, thumbstick_x},
	        {d::LEFT_THUMBSTICK_Y, 0, thumbstick_y},       {d::RIGHT_THUMBSTICK_Y, 1, thumbstick_y},
	        {d::LEFT_THUMBSTICK_CLICK, 0, thumbstick_click}, {d::RIGHT_THUMBSTICK_CLICK, 1, thumbstick_click},
	        {d::LEFT_THUMBSTICK_TOUCH, 0, thumbstick_touch}, {d::RIGHT_THUMBSTICK_TOUCH, 1, thumbstick_touch},
	        {d::X_CLICK, 0, a_click},                      {d::A_CLICK, 1, a_click},
	        {d::X_TOUCH, 0, a_touch},                      {d::A_TOUCH, 1, a_touch},
	        {d::Y_CLICK, 0, b_click},                      {d::B_CLICK, 1, b_click},
	        {d::Y_TOUCH, 0, b_touch},                      {d::B_TOUCH, 1, b_touch},
	        {d::MENU_CLICK, 0, menu_click},                {d::SYSTEM_CLICK, 1, system_click},
	        {d::LEFT_THUMBREST_TOUCH, 0, thumbrest_touch}, {d::RIGHT_THUMBREST_TOUCH, 1, thumbrest_touch},
	        {d::LEFT_TRACKPAD_X, 0, trackpad_x},           {d::RIGHT_TRACKPAD_X, 1, trackpad_x},
	        {d::LEFT_TRACKPAD_Y, 0, trackpad_y},           {d::RIGHT_TRACKPAD_Y, 1, trackpad_y},
	        {d::LEFT_TRACKPAD_TOUCH, 0, trackpad_touch},   {d::RIGHT_TRACKPAD_TOUCH, 1, trackpad_touch},
	        {d::LEFT_TRACKPAD_FORCE, 0, trackpad_force},   {d::RIGHT_TRACKPAD_FORCE, 1, trackpad_force},
	};
	for (const auto & e: table)
		if (uint32_t(e.id) == wivrn_id)
		{
			hand = e.hand;
			index = e.index;
			return true;
		}
	return false;
}
} // namespace vrhost
