// SPDX-License-Identifier: GPL-3.0-or-later
// Adaptive release of the frame clock (--frames-in-flight auto): the next frame goes to the guest when its CPU work
// will end just as the GPU finishes the frame before it, like SteamVR's running start and Meta's adaptive queue-ahead.
//
// Why: a GPU-bound game (VRChat on an M2 at 20-35 fps) that wakes on the pacer's grid samples its poses, does its CPU
// work, then waits for the GPU behind the previous frame (DXMT bounds the queue at one frame): the pose ages by that
// wait. A fixed limit of one frame in flight removes the wait but serialises CPU and GPU (live: 124 -> 96 ms pose to
// headset, but 23 -> 16 fps). Released at "previous frame's predicted GPU end - CPU time", the CPU work overlaps the
// GPU tail exactly: the latency of the limit at the frame rate of no limit.
//
// Only a GPU-bound game is held. One that is not (CPU >= GPU + margin) gets a timing on every pacer tick, as without
// a limit: it takes the newest when it asks, so its pose is never older than a display period, whereas one timing per
// frame, released before the game is ready for it, would age until the game takes it.
//
// From the frames that come back, per frame (host clock): CPU = submit - release (the game woke on the release and
// submitted), GPU = copied - max(submit, previous copied) (DXMT commits the frame's work at the export flush, so the
// GPU runs it from the later of the submit and the end of the previous frame). Both are medians over the last frames,
// so a hitch moves them little. Neither is biased by the hold itself: a late release adds an idle GPU gap that the
// GPU measure leaves out, an early one makes the frame wait in the GPU queue, which neither measure includes. A frame
// the game picked up after its release (not held, not waiting) counts that delay as CPU time, which only makes a
// hold less likely. The caller (FramesInFlight) bounds every hold and does not hold while the game renders nothing.
#pragma once
#include "session/vr_session.h" // ns

#include <algorithm>
#include <array>
#include <cstdint>

namespace vrhost
{
class AdaptiveRelease
{
public:
	static constexpr size_t window = 9;              // frames per estimate (median and quartiles)
	static constexpr size_t min_samples = 3;         // fewer: no estimate, no hold
	static constexpr ns base_margin_ns = 2'000'000;  // released this much early, plus half the CPU and GPU jitter

	// The clock sent frame `id` to the guest at `t`.
	void released(uint64_t id, ns t)
	{
		rel[id % rel.size()] = {id, t};
	}

	// Frame `id` came back: the game submitted it at `submit` and its image was copied (GPU done) at `copied`.
	void frame(uint64_t id, ns submit, ns copied)
	{
		if (id <= last_id or copied < submit)
			return;
		const Rel & r = rel[id % rel.size()];
		if (r.id == id and submit >= r.t)
			push(cpu, submit - r.t);
		push(gpu, copied - std::max(submit, last_copied));
		last_id = id;
		last_copied = copied;
	}

	// When to release the frame after `issued` (the last frame sent); at or before now = at once.
	ns release_time(uint64_t issued) const
	{
		if (not gpu_bound())
			return 0;
		ns c = cpu.median(), g = gpu.median();
		// predicted GPU end of each frame sent after the last one that came back, in order
		ns done = last_copied;
		for (uint64_t id = last_id + 1; id <= issued; id++)
		{
			const Rel & r = rel[id % rel.size()];
			if (r.id != id)
				continue; // not sent (pacer ids may skip) or too old to matter
			done = std::max(r.t + c, done) + g;
		}
		return done - c - margin();
	}

	bool gpu_bound() const
	{
		return cpu.n >= min_samples and gpu.n >= min_samples and last_copied and gpu.median() > cpu.median() + margin();
	}
	ns cpu_ns() const { return cpu.n >= min_samples ? cpu.median() : 0; }
	ns gpu_ns() const { return gpu.n >= min_samples ? gpu.median() : 0; }
	ns margin() const { return base_margin_ns + (cpu.spread() + gpu.spread()) / 2; }

private:
	struct Rel
	{
		uint64_t id = 0;
		ns t = 0;
	};
	struct Window
	{
		std::array<ns, window> v{};
		size_t n = 0, pos = 0;
		std::array<ns, window> sorted() const
		{
			std::array<ns, window> s = v;
			std::sort(s.begin(), s.begin() + n);
			return s;
		}
		ns median() const { return sorted()[n / 2]; }
		ns spread() const // interquartile range
		{
			if (n < min_samples)
				return 0;
			auto s = sorted();
			return s[(3 * n) / 4] - s[n / 4];
		}
	};
	static void push(Window & w, ns x)
	{
		w.v[w.pos] = x;
		w.pos = (w.pos + 1) % window;
		w.n = std::min(w.n + 1, window);
	}

	std::array<Rel, 16> rel{};
	Window cpu, gpu;
	uint64_t last_id = 0;
	ns last_copied = 0;
};
} // namespace vrhost
