// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdio>
#include <cstdlib>

inline int g_failures = 0;
#define CHECK(cond)                                                                      \
	do                                                                                   \
	{                                                                                    \
		if (!(cond))                                                                     \
		{                                                                                \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
			++g_failures;                                                                \
		}                                                                                \
	} while (0)
#define REQUIRE(cond)                                                                    \
	do                                                                                   \
	{                                                                                    \
		if (!(cond))                                                                     \
		{                                                                                \
			fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #cond);   \
			exit(1);                                                                     \
		}                                                                                \
	} while (0)
inline int test_result(const char * name)
{
	if (g_failures)
		fprintf(stderr, "%s: %d failure(s)\n", name, g_failures);
	else
		printf("%s: ok\n", name);
	return g_failures ? 1 : 0;
}
