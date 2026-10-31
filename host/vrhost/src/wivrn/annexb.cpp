// SPDX-License-Identifier: GPL-3.0-or-later
#include "wivrn/annexb.h"
#include <string_view>

namespace vrhost::annexb
{
void append_nal(std::vector<uint8_t> & out, std::span<const uint8_t> nal)
{
	static const uint8_t start[] = {0, 0, 0, 1};
	out.insert(out.end(), std::begin(start), std::end(start));
	out.insert(out.end(), nal.begin(), nal.end());
}

bool append_length_prefixed(std::vector<uint8_t> & out, std::span<const uint8_t> data, int length_size)
{
	if (length_size != 1 and length_size != 2 and length_size != 4)
		return false;
	size_t pos = 0;
	while (pos < data.size())
	{
		if (data.size() - pos < size_t(length_size))
			return false;
		size_t len = 0;
		for (int i = 0; i < length_size; ++i)
			len = (len << 8) | data[pos + i];
		pos += length_size;
		if (len == 0 or len > data.size() - pos)
			return false;
		append_nal(out, data.subspan(pos, len));
		pos += len;
	}
	return true;
}

std::vector<std::span<const uint8_t>> split(std::span<const uint8_t> s)
{
	std::vector<std::span<const uint8_t>> nals;
	size_t i = 0, begin = std::string_view::npos;
	auto start_code_at = [&](size_t p) -> size_t {
		if (p + 3 <= s.size() and s[p] == 0 and s[p + 1] == 0 and s[p + 2] == 1)
			return 3;
		if (p + 4 <= s.size() and s[p] == 0 and s[p + 1] == 0 and s[p + 2] == 0 and s[p + 3] == 1)
			return 4;
		return 0;
	};
	while (i < s.size())
	{
		if (size_t n = start_code_at(i))
		{
			if (begin != std::string_view::npos)
				nals.push_back(s.subspan(begin, i - begin));
			i += n;
			begin = i;
		}
		else
			++i;
	}
	if (begin != std::string_view::npos and begin < s.size())
		nals.push_back(s.subspan(begin));
	return nals;
}
} // namespace vrhost::annexb
