// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/pcm.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace vrhost::pcm
{
void remix(const int16_t * in, uint32_t in_ch, int16_t * out, uint32_t out_ch, size_t frames)
{
	if (in_ch == out_ch)
	{
		memmove(out, in, frames * in_ch * sizeof(int16_t));
		return;
	}
	for (size_t f = 0; f < frames; ++f)
	{
		const int16_t * i = in + f * in_ch;
		int16_t * o = out + f * out_ch;
		if (in_ch == 1)
			std::fill(o, o + out_ch, i[0]);
		else if (out_ch == 1)
			o[0] = int16_t((int32_t(i[0]) + int32_t(i[1])) / 2);
		else
			for (uint32_t c = 0; c < out_ch; ++c)
				o[c] = c < in_ch ? i[c] : 0;
	}
}

void Resampler::configure(uint32_t in, uint32_t out, uint32_t ch)
{
	if (in == in_rate and out == out_rate and ch == channels)
		return;
	in_rate = in ? in : 48000;
	out_rate = out ? out : 48000;
	channels = ch;
	reset();
}

void Resampler::reset()
{
	pos = 0;
	prev.assign(channels, 0);
}

void Resampler::process(std::span<const int16_t> in, std::vector<int16_t> & out)
{
	const size_t n = in.size() / channels;
	if (n == 0)
		return;
	if (passthrough())
	{
		out.insert(out.end(), in.begin(), in.begin() + n * channels);
		return;
	}
	// Input frame i of this block is x(i); x(-1) is the last frame of the previous block. `pos` is the position of
	// the next output frame on that axis (> -1 between blocks, 0 before the first one).
	auto x = [&](ptrdiff_t i, uint32_t c) -> double { return i < 0 ? prev[c] : in[i * channels + c]; };
	const double step = double(in_rate) / out_rate;
	while (pos <= double(n - 1))
	{
		ptrdiff_t i = ptrdiff_t(std::floor(pos));
		double f = pos - double(i);
		for (uint32_t c = 0; c < channels; ++c)
		{
			double a = x(i, c), b = f > 0 ? x(i + 1, c) : a;
			out.push_back(int16_t(std::lround(std::clamp(a + (b - a) * f, -32768.0, 32767.0))));
		}
		pos += step;
	}
	pos -= double(n);
	std::copy(in.begin() + (n - 1) * channels, in.begin() + n * channels, prev.begin());
}

static uint64_t load(const uint64_t & v, std::memory_order o)
{
	return std::atomic_ref<uint64_t>(const_cast<uint64_t &>(v)).load(o);
}
static void store(uint64_t & v, uint64_t x)
{
	std::atomic_ref<uint64_t>(v).store(x, std::memory_order_release);
}

size_t ring_available(const vra_ring & r)
{
	return size_t(load(r.write, std::memory_order_acquire) - load(r.read, std::memory_order_acquire));
}

size_t ring_read(vra_ring & r, const int16_t * data, int16_t * dst, size_t max_frames)
{
	uint64_t rd = load(r.read, std::memory_order_relaxed);
	size_t n = std::min<size_t>(max_frames, load(r.write, std::memory_order_acquire) - rd);
	const uint32_t cap = r.capacity, ch = r.channels;
	size_t done = 0;
	while (done < n)
	{
		size_t off = (rd + done) & (cap - 1);
		size_t k = std::min<size_t>(n - done, cap - off);
		memcpy(dst + done * ch, data + off * ch, k * ch * sizeof(int16_t));
		done += k;
	}
	store(r.read, rd + n);
	return n;
}

size_t ring_skip(vra_ring & r, size_t frames)
{
	uint64_t rd = load(r.read, std::memory_order_relaxed);
	size_t n = std::min<size_t>(frames, load(r.write, std::memory_order_acquire) - rd);
	store(r.read, rd + n);
	return n;
}

size_t ring_write(vra_ring & r, int16_t * data, const int16_t * src, size_t frames)
{
	uint64_t wr = load(r.write, std::memory_order_relaxed);
	const uint32_t cap = r.capacity, ch = r.channels;
	size_t free_frames = cap - size_t(wr - load(r.read, std::memory_order_acquire));
	size_t n = std::min(frames, free_frames);
	size_t done = 0;
	while (done < n)
	{
		size_t off = (wr + done) & (cap - 1);
		size_t k = std::min<size_t>(n - done, cap - off);
		memcpy(data + off * ch, src + done * ch, k * ch * sizeof(int16_t));
		done += k;
	}
	store(r.write, wr + n);
	if (n < frames)
		std::atomic_ref<uint64_t>(r.overruns).fetch_add(frames - n, std::memory_order_relaxed);
	return n;
}
} // namespace vrhost::pcm
