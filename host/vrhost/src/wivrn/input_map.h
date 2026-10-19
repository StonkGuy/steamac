// SPDX-License-Identifier: GPL-3.0-or-later
// WiVRn input ids (InputValue::id = WiVRn device_id) -> the guest's adapter-neutral input table (vrlink.h vrl_input,
// enum svr_input in guest/vr/include/steamac_vr_guest.h). Kept in src/wivrn/ so no WiVRn header leaks out.
#pragma once
#include <cstdint>

namespace vrhost
{
// hand: 0 left, 1 right; index: svr_input id. False for ids the guest table does not carry.
bool wivrn_input_to_vrl(uint32_t wivrn_id, int & hand, int & index);
} // namespace vrhost
