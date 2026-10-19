// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal stand-in for Monado's util/u_var.h (debug GUI variable tracking): the types WiVRn's pacer uses, and
// no-op registration (steamac-vrhost has no Monado debug GUI).
#pragma once
struct u_var_draggable_f32
{
	float val;
	float step;
	float min;
	float max;
};
inline void u_var_add_root(void *, const char *, bool) {}
inline void u_var_remove_root(void *) {}
inline void u_var_add_draggable_f32(void *, u_var_draggable_f32 *, const char *) {}
