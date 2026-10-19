// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer target: one headset message (control or stream payload after framing/decryption) through WiVRn's
// deserializer, as steamac-vrhost decodes it from the network.
#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <cstring>
#include <memory>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size)
{
	auto buf = std::make_shared<uint8_t[]>(size ? size : 1);
	if (size)
		memcpy(buf.get(), data, size);
	wivrn::deserialization_packet p{buf, std::span<uint8_t>(buf.get(), size)};
	try
	{
		auto v = p.deserialize<wivrn::from_headset::packets>();
		(void)v;
	}
	catch (std::exception &)
	{
	}
	return 0;
}
