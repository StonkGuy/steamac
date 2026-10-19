// SPDX-License-Identifier: GPL-3.0-or-later
// Game path of steamac-vrhost (docs/vr-interfaces.md sections 2-4):
// * frames: VR link (VrLinkServer) -> CompositorBridge (foveation + '420f') -> SessionEncoderSink -> VrSession::submit
//   (WiVRn: VtEncoder per eye, shards, foveation parameters in view_info). Frames that arrive while no headset is
//   connected are released without work.
// * to the guest's OpenVR runtime: VRL_STATE on connect/disconnect/refresh change, VRL_TIMING (the pacer's next frame)
//   + VRL_POSES (predicted to its display time) once per frame from a frame-clock thread, VRL_INPUT on change;
//   VRL_HAPTICS from the guest -> VrSession::haptics. The pipeline is the session's listener: construct it before
//   VrSession::start(&pipeline).
#pragma once
#include "compositor/vrlink_server.h"
#include "vrlink.h"
#include "session/vr_session.h"

#include <array>
#include <functional>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace vrhost
{
// EyeEncoderSink over a VrSession: buffers come from a '420f' IOSurface pool of the stream size; when both eyes are
// composed the frame is submitted with the link frame's id, display time, poses, FOVs and the foveation parameters.
class SessionEncoderSink final : public EyeEncoderSink
{
public:
	SessionEncoderSink(VrSession & session, uint32_t w, uint32_t h);
	~SessionEncoderSink() override;
	CVPixelBufferRef acquire(int eye) override;
	void encode(int eye, CVPixelBufferRef buffer, const LinkFrame & frame, const FoveationParams & params) override;

private:
	VrSession & session;
	CVPixelBufferPoolRef pool = nullptr;
	CVPixelBufferRef pending[2] = {};
	FoveationParams params[2];
};

struct PipelineStats
{
	std::atomic<uint64_t> received{0}, submitted{0}, no_headset{0};
	std::atomic<int64_t> last_compose_gpu_ns{0}, last_latency_ns{0}; // app submit -> encoded + sent
};

// Per-frame latencies (ms, host clock) of the frames since the last take(): WMTVRSubmit (call 146 on the executor) ->
// copy complete -> received by the host -> composed, encoded and sent.
struct PipelineLatency
{
	std::vector<double> submit_to_copied, copied_to_received, received_to_sent, total, compose_gpu;
};

class VrPipeline final : public SessionListener
{
public:
	// link_fd: AF_UNIX SOCK_DGRAM end (owned); mach_name: bootstrap name for the swapchain ports.
	// opt: Settings > VR quality options; the upscale size is taken from each connection's nominal render size.
	VrPipeline(VrSession & session, int link_fd, const std::string & mach_name, CompositorOptions opt = {});
	~VrPipeline() override;
	bool peer_closed() const;
	const PipelineStats & stats() const { return st; }
	PipelineLatency take_latency();

	// SessionListener
	void on_connected(const HeadsetInfo &) override;
	void on_disconnected() override;
	void on_input(std::span<const InputValue>) override;
	void on_refresh_rate(float hz) override;

	uint64_t timings_sent() const { return timings; }
	const char * upscaler() const; // of the last composed frame ("" = none)
	uint64_t haptics_routed() const { return haptics; }

private:
	void on_frame(const LinkFrame &);
	void on_frame_error();
	void on_message(const void *, size_t);
	void send_state(bool connected);
	static vrl_state default_view(); // the view a game gets before any headset connected
	vrl_state last_view{};           // the last connected headset's view (send_state)
	void send_input();
	void frame_clock();
	VrSession & session;
	std::mutex m;
	uint32_t w = 0, h = 0, up_w = 0;
	std::array<uint32_t, 6> last_geometry{}; // eye image sizes, format, flags of the last frame (logged on change)
	std::unique_ptr<SessionEncoderSink> sink;
	std::unique_ptr<CompositorBridge> bridge;
	CompositorOptions opt;
	std::atomic<const char *> last_upscaler{""};
	PipelineStats st;
	std::mutex lat_mutex;
	PipelineLatency lat;
	std::mutex input_mutex;
	float input[2][21] = {}; // VRL_INPUT_COUNT
	int64_t input_time = 0;
	std::atomic<bool> quit{false};
	std::atomic<uint64_t> timings{0}, haptics{0};
	std::atomic<float> refresh{0};
	std::unique_ptr<VrLinkServer> server; // after the state its threads use
	std::thread clock;                     // last: started when everything else exists
};
} // namespace vrhost
