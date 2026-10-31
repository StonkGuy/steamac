// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal stand-in for Monado's util/u_debug.h: DEBUG_GET_ONCE_FLOAT_OPTION reads an environment variable once.
#pragma once
#include <cstdlib>
#define DEBUG_GET_ONCE_FLOAT_OPTION(suffix, name, default_value)                                                       \
	static float debug_get_float_option_##suffix()                                                                     \
	{                                                                                                                  \
		static const float v = [] {                                                                                    \
			const char * s = std::getenv(name);                                                                        \
			return s ? std::strtof(s, nullptr) : float(default_value);                                                 \
		}();                                                                                                           \
		return v;                                                                                                      \
	}
