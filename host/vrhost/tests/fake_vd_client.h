// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only Virtual Desktop headset, written from the private interoperability notes (PROTOCOL.md), not from vendor
// code: discovery offer with its own RSA key, then the four TCP channels to the computer record's ports; sends its
// mobile settings, tracking/input, microphone PCM and a key-frame request; receives the streamer settings, video format,
// frames (decoded with VideoToolbox), haptics in the frame metadata and speaker PCM. Shares the wire code with the
// host (src/vd), so it checks the adapter's behaviour and the pinned assumptions, not the vendor's real layout.
#pragma once

#include "vd/vd_discovery.h"
#include "vd/vd_wire.h"

#include <CoreVideo/CoreVideo.h>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct FakeVdOptions
{
	std::string host = "127.0.0.1";
	uint16_t discovery_port = 0;
	vrhost::vd::AesKey discovery_key; // test fixture: random per run, never a real key
	std::string account = "test-account";
	uint16_t platform = 1;
	std::string mobile_settings = R"({"HmdType":"Quest3","Framerate":90,"UseSpacewarp":false,"VideoCodec":"HEVC","Bitrate":150})";
	int frames = 10; // frames wanted (both eyes decoded)
	int timeout_ms = 20000;
	bool audio = true;
	std::optional<vrhost::vd::FrameData> frame_data; // sent after each received frame
	int force_key_frame_after = -1;                 // send ForceKeyFrame after this many frames
	std::vector<int16_t> microphone;                // sent once, after the video format arrived
	std::vector<std::pair<std::string, std::string>> mobile_changes; // sent after the first frame
	bool decode = true;
	bool annexb = false; // expect Annex-B (sent UseAnnexB=true) instead of length-prefixed NALs
};

struct FakeVdResult
{
	std::string error; // empty on success
	std::optional<vrhost::vd::ComputerRecord> record;
	bool peer_encrypt = false; // host PeerInfo flag
	std::string streamer_settings;
	std::vector<std::pair<std::string, std::string>> streamer_changes;
	std::vector<vrhost::vd::VideoFormat> formats;
	std::vector<vrhost::vd::FrameMetadata> frames;
	std::vector<vrhost::vd::SliceHeader> slices;
	std::array<int, 2> decoded{}, idr{};
	int length_prefix_errors = 0; // eye data that is not a clean run of 4-byte big-endian length-prefixed NALs
	std::vector<int16_t> speaker;
	int speaker_packets = 0;
};

// Discovery only: one offer, the reply if the host answered within timeout_ms.
std::optional<vrhost::vd::Reply> fake_vd_discover(const FakeVdOptions &, int timeout_ms = 2000);
FakeVdResult run_fake_vd_client(const FakeVdOptions &);
