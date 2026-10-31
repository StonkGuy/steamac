// SPDX-License-Identifier: GPL-3.0-or-later
// S16 interleaved PCM helpers for the VR audio bridge: channel remix, a streaming linear resampler, and the
// single-producer single-consumer access to the shared vra_ring (include/vraudio.h).
#pragma once

#include "vraudio.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vrhost::pcm
{
// in_ch -> out_ch, `frames` frames. Mono -> N duplicates, N -> mono averages the first two channels, otherwise the
// common channels are copied and the rest is silence.
void remix(const int16_t * in, uint32_t in_ch, int16_t * out, uint32_t out_ch, size_t frames);

// Streaming linear-interpolation resampler (rate changes are rare: guest and headset both run 48 kHz in practice).
// Keeps the last input frame and the fractional position between calls, so consecutive blocks join seamlessly.
class Resampler
{
public:
	void configure(uint32_t in_rate, uint32_t out_rate, uint32_t channels);
	bool passthrough() const { return in_rate == out_rate; }
	// Appends the resampled frames to `out`.
	void process(std::span<const int16_t> in, std::vector<int16_t> & out);
	void reset();

private:
	uint32_t in_rate = 48000, out_rate = 48000, channels = 2;
	double pos = 0;            // position of the next output frame in input frames (see process())
	std::vector<int16_t> prev = std::vector<int16_t>(2); // last input frame of the previous block
};

// --- vra_ring (SPSC, frames of ring.channels S16 samples) ---
size_t ring_available(const vra_ring &);
// Consumer: copies up to `max_frames` frames into `dst`. Returns frames read.
size_t ring_read(vra_ring &, const int16_t * data, int16_t * dst, size_t max_frames);
// Consumer: drops `frames` frames (at most what is available). Returns frames dropped.
size_t ring_skip(vra_ring &, size_t frames);
// Producer: writes what fits, counts the rest in `overruns`. Returns frames written.
size_t ring_write(vra_ring &, int16_t * data, const int16_t * src, size_t frames);
} // namespace vrhost::pcm
