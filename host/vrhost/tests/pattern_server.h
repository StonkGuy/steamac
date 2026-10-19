// SPDX-License-Identifier: GPL-3.0-or-later
// A WivrnSession on loopback (port 0, no Bonjour) with the test-pattern producer of `steamac-vrhost --testpattern`,
// optionally behind the VR audio bridge; shared by the loopback and control-socket tests.
#pragma once
#include "audio/audio_bridge.h"
#include "pattern/test_pattern.h"
#include "session/clock.h"
#include "wivrn/wivrn_session.h"

#include <CoreVideo/CoreVideo.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <cstdio>
#include <thread>

using namespace vrhost;

inline CVPixelBufferRef make_nv12(uint32_t w, uint32_t h)
{
	CFDictionaryRef empty = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void * k[] = {kCVPixelBufferIOSurfacePropertiesKey, kCVPixelBufferMetalCompatibilityKey};
	const void * v[] = {empty, kCFBooleanTrue};
	CFDictionaryRef attrs = CFDictionaryCreate(nullptr, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CVPixelBufferRef buf = nullptr;
	CVPixelBufferCreate(nullptr, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, attrs, &buf);
	CFRelease(attrs);
	CFRelease(empty);
	return buf;
}


// A WivrnSession with the test-pattern producer of `steamac-vrhost --testpattern`.
struct Server
{
	WivrnSession session;
	std::unique_ptr<AudioBridge> audio; // optional, wraps the session listener as in steamac-vrhost --audio-fd
	std::atomic<bool> stop{false};
	std::atomic<int> skip_every{0}; // > 0: every n-th frame timing gets no submit
	int timings = 0;
	std::thread producer;

	static WivrnConfig local(WivrnConfig cfg)
	{
		cfg.port = 0;
		cfg.advertise = false;
		cfg.log = [](const std::string & s) { printf("[server] %s\n", s.c_str()); };
		return cfg;
	}
	explicit Server(WivrnConfig cfg, vra_region * audio_region = nullptr) :
	        session(local(std::move(cfg)))
	{
		if (audio_region)
			audio = std::make_unique<AudioBridge>(audio_region, session, nullptr);
		session.start(audio.get());
		producer = std::thread([this] {
			TestPattern pattern;
			std::array<CVPixelBufferRef, 2> bufs{};
			while (not stop)
			{
				auto info = session.headset();
				if (not session.connected() or not info)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}
				if (not bufs[0])
					for (auto & b: bufs)
						b = make_nv12(info->eye_width, info->eye_height);
				FrameTiming t = session.next_frame_timing();
				if (skip_every and ++timings % skip_every == 0)
					continue; // a game that misses a frame: timing asked, nothing submitted
				Frame f;
				f.frame_id = t.frame_id;
				f.display_time = t.predicted_display;
				session.views_at(t.predicted_display, f.pose, f.fov);
				for (int i = 0; i < 2; ++i)
				{
					pattern.render(bufs[i], f.pose[i], f.fov[i], f.frame_id);
					f.eye[i].surface = CVPixelBufferGetIOSurface(bufs[i]);
				}
				session.submit(f);
			}
			for (auto b: bufs)
				if (b)
					CVPixelBufferRelease(b);
		});
	}
	~Server()
	{
		stop = true;
		producer.join();
		session.stop();
		audio.reset();
	}
	void wait_disconnect()
	{
		for (int i = 0; i < 100 and session.connected(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
};

