// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only Virtual Desktop headset, written from the private evidence ledger (not from vendor code): discovery offer
// with its own 1024-bit RSA key, the reply (wrapper key + computer record with the channel key), "connect now", then
// the four TCP channels in the Streamer's order (control, video, audio, data) to the ports the test names (the real
// headset uses the fixed ports). Sends its user + mobile settings, tracking/input, microphone PCM, a key-frame
// request, UpdateRequired and WakeUp; receives the streamer settings, the bandwidth probe, the video format, frames
// (decoded with VideoToolbox), haptics in the frame metadata, host commands and speaker PCM. Shares the wire code
// with the host (src/vd), so it checks the adapter's behaviour against the pinned facts.
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
	uint16_t discovery_port = 0, control_port = 0, data_port = 0, video_port = 0, audio_port = 0;
	vrhost::vd::AesKey discovery_key; // test fixture: random per run, never a real key
	std::string account = "test-account";
	uint8_t platform = 1;
	std::string user_settings = R"({"VRFramerate":90,"VRGraphicsQuality":0})";
	std::string mobile_settings = R"({"HmdType":1000,"UseSpacewarp":false,"IPD":0.066,"MeasuredBandwidth":150000000,)"
	                              R"("FovLeft":{"UpTan":1.0,"DownTan":1.2,"LeftTan":1.3,"RightTan":0.9},)"
	                              R"("FovRight":{"UpTan":1.0,"DownTan":1.2,"LeftTan":0.9,"RightTan":1.3},"Features":262144})";
	int frames = 10; // frames wanted (both eyes decoded)
	int timeout_ms = 20000;
	bool audio = true;
	std::optional<vrhost::vd::TrackingSample> tracking; // sent after each received frame, frame_index = count
	size_t tracking_size = vrhost::vd::frame_data_base;
	int force_key_frame_after = -1; // send ForceKeyFrame after this many frames
	std::vector<int16_t> microphone; // sent once, after the video format arrived
	std::vector<std::pair<std::string, std::string>> user_changes, mobile_changes; // sent after the first frame
	bool decode = true;
	bool annexb = true; // expect Annex-B (default) or 4-byte BE length-prefixed NALs (UseAnnexB=false)
	bool extra_requests = false; // UpdateRequired + WakeUpComputer after the first frame
};

struct FakeVdResult
{
	std::string error; // empty on success
	std::optional<vrhost::vd::ComputerRecord> record;
	std::string streamer_settings;
	std::vector<std::pair<std::string, std::string>> streamer_changes; // {"key": value} split
	std::vector<uint8_t> control_types;                                // every control message type received
	std::optional<int32_t> bandwidth_probe;
	std::vector<vrhost::vd::VideoFormat> formats;
	std::vector<vrhost::vd::FrameMetadata> frames;
	std::vector<vrhost::vd::SliceHeader> slices;
	std::array<int, 2> decoded{}, idr{};
	int framing_errors = 0; // eye data not in the expected NAL framing
	std::vector<int16_t> speaker;
	int speaker_packets = 0, speaker_max_packet = 0;
};

// Discovery only: one offer, the record if the host answered within timeout_ms.
std::optional<vrhost::vd::ComputerRecord> fake_vd_discover(const FakeVdOptions &, int timeout_ms = 2000);
// "connect now" datagram for a record.
void fake_vd_connect_now(const FakeVdOptions &, const vrhost::vd::ComputerRecord &);
FakeVdResult run_fake_vd_client(const FakeVdOptions &, const std::function<void()> & after_first_frame = nullptr);
