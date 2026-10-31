// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal stand-in for Monado's main/comp_target.h: only what WiVRn's pacer (server/compositor/pacer.*) uses.
#pragma once
enum comp_target_timing_point
{
	COMP_TARGET_TIMING_POINT_WAKE_UP,     //!< Woke up after sleeping in wait frame.
	COMP_TARGET_TIMING_POINT_BEGIN,       //!< Began CPU side work for GPU.
	COMP_TARGET_TIMING_POINT_SUBMIT_BEGIN, //!< Just before submitting work to the GPU.
	COMP_TARGET_TIMING_POINT_SUBMIT_END,  //!< Just after submitting work to the GPU.
};
