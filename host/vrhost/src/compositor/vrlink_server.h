// SPDX-License-Identifier: GPL-3.0-or-later
// steamac-vrhost end of the VR link (include/vrlink.h, docs/vr-interfaces.md section 3): receives swapchain
// IOSurfaces over Mach and frame submits over the link socket, hands each frame to a sink, and
// returns the images with VRL_RELEASE when the sink is done with them.
#pragma once
#include "eye_compositor.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace vrhost
{
struct LinkFrame
{
	uint64_t frame_id;
	int64_t display_time, app_submit_time, ready_time, received_time; // host ns
	EyeSource eye[2];
	EyeFov fov[2];
	float pose[2][7]; // orientation xyzw, position xyz
};

class VrLinkServer
{
public:
	// Registers `mach_name` (bootstrap) and serves `link_fd` (AF_UNIX SOCK_DGRAM socketpair end, owned). The sink runs on the
	// server thread; images are released when it returns.
	using Sink = std::function<void(const LinkFrame &)>;
	using SinkError = std::function<void()>;
	// Other vm -> host packets (VRL_HAPTICS, ...): the whole datagram (struct vrl_header first), on the server thread.
	using MessageHandler = std::function<void(const void *, size_t)>;
	VrLinkServer(int link_fd, const std::string & mach_name, Sink sink, MessageHandler other = {}, SinkError on_sink_error = {});
	~VrLinkServer();
	VrLinkServer(const VrLinkServer &) = delete;
	VrLinkServer & operator=(const VrLinkServer &) = delete;

	// Listener for wmtr_vr_connect at `path` (tests/manual runs): returns the link fd, or -1 on timeout.
	static int accept_one(const std::string & path, int timeout_ms);

	// host -> vm packet (one struct per datagram); any thread. False when the peer is gone or the socket is full.
	bool send(const void * msg, size_t size);

	uint64_t frames() const;
	bool peer_closed() const;

private:
	struct impl;
	std::unique_ptr<impl> p;
};

// Bridge from link frames to the encoder: compose both eyes (EyeCompositor) into encoder buffers and hand
// them over. The encoder side is an interface so the M0 VtEncoder and a test stub both fit.
class EyeEncoderSink
{
public:
	virtual ~EyeEncoderSink() = default;
	// an output buffer for eye `eye` ('420f', IOSurface-backed, out_w x out_h); retained, released by the bridge
	virtual CVPixelBufferRef acquire(int eye) = 0;
	// synchronous like VtEncoder::encode; may keep (retain) the buffer
	virtual void encode(int eye, CVPixelBufferRef buffer, const LinkFrame & frame, const FoveationParams & params) = 0;
};

class CompositorBridge
{
public:
	CompositorBridge(uint32_t out_w, uint32_t out_h, EyeEncoderSink & encoder, CompositorOptions = {});
	const char * last_upscaler() const { return compositor.last_upscaler(); }
	void operator()(const LinkFrame & frame);
	// timings of the last frame (host ns)
	struct Timing
	{
		int64_t compose_ns, compose_gpu_ns, encode_ns;
	};
	Timing last_timing() const { return timing; }
	// each eye's x offset from the head centre in head space (WiVRn foveation eye_x): -/+ half the eye distance
	static void eye_offsets(const LinkFrame & frame, float eye_x[2]);

private:
	EyeCompositor compositor;
	EyeEncoderSink & encoder;
	Timing timing{};
};
} // namespace vrhost
