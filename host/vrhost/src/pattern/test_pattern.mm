// SPDX-License-Identifier: GPL-3.0-or-later
#include "pattern/test_pattern.h"

#import <Metal/Metal.h>
#include <cmath>
#include <stdexcept>

namespace vrhost
{
static const char * kernel_src = R"(
#include <metal_stdlib>
using namespace metal;

struct params {
	float4 q;       // eye orientation (world from eye), xyzw
	float4 tan_fov; // left, right, up, down (tangents)
	uint frame;
	uint bit_block;
	uint bits;
};

static float3 qrot(float4 q, float3 v) {
	float3 t = 2.0 * cross(q.xyz, v);
	return v + q.w * t + cross(q.xyz, t);
}

static float3 hue(float h) {
	float3 k = float3(1.0, 2.0 / 3.0, 1.0 / 3.0);
	return clamp(abs(fract(h + k) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

static float3 shade(float2 uv, constant params & p, uint2 px) {
	// head-locked frame index blocks, top-left
	if (px.y < p.bit_block && px.x < p.bit_block * p.bits) {
		uint bit = px.x / p.bit_block;
		return ((p.frame >> bit) & 1u) ? float3(1.0) : float3(0.0);
	}
	float tx = mix(p.tan_fov.x, p.tan_fov.y, uv.x);
	float ty = mix(p.tan_fov.z, p.tan_fov.w, uv.y);
	float3 d = normalize(qrot(p.q, normalize(float3(tx, ty, -1.0))));
	float yaw = atan2(d.x, -d.z);          // 0 = world forward
	float pitch = asin(clamp(d.y, -1.0, 1.0));
	float deg = 57.29578;
	float3 c = hue(floor((yaw * deg + 180.0) / 45.0) / 8.0) * (pitch < 0.0 ? 0.35 : 0.6);
	float gy = abs(fract(yaw * deg / 15.0 + 0.5) - 0.5) * 15.0;
	float gp = abs(fract(pitch * deg / 15.0 + 0.5) - 0.5) * 15.0;
	if (min(gy, gp) < 0.25) c = float3(0.9);
	if (abs(pitch * deg) < 0.4) c = float3(1.0, 0.9, 0.0);
	float r = acos(clamp(-d.z, -1.0, 1.0)) * deg; // angle from forward
	if (r < 3.0 && r > 2.4) c = float3(1.0, 0.0, 0.0);
	if (r < 3.0 && (abs(yaw * deg) < 0.25 || abs(pitch * deg) < 0.25)) c = float3(1.0, 0.0, 0.0);
	return c;
}

static float3 ycbcr709(float3 c) {   // full range, same matrix as WiVRn foveation.comp
	float y = dot(c, float3(0.2126, 0.7152, 0.0722));
	return float3(y, (c.b - y) / 1.8556 + 0.5, (c.r - y) / 1.5748 + 0.5);
}

kernel void pattern(texture2d<float, access::write> luma [[texture(0)]],
                    texture2d<float, access::write> chroma [[texture(1)]],
                    constant params & p [[buffer(0)]],
                    uint2 gid [[thread_position_in_grid]]) {
	uint w = luma.get_width(), h = luma.get_height();
	if (gid.x >= w || gid.y >= h) return;
	float2 uv = (float2(gid) + 0.5) / float2(w, h);
	float3 ycc = ycbcr709(shade(uv, p, gid));
	luma.write(float4(ycc.x), gid);
	if ((gid.x & 1u) == 0 && (gid.y & 1u) == 0)
		chroma.write(float4(ycc.y, ycc.z, 0, 0), gid / 2);
}
)";

struct params
{
	float q[4];
	float tan_fov[4];
	uint32_t frame;
	uint32_t bit_block;
	uint32_t bits;
};

struct TestPattern::impl
{
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	id<MTLComputePipelineState> pipeline;
	CVMetalTextureCacheRef cache = nullptr;
};

TestPattern::TestPattern() :
        p(std::make_unique<impl>())
{
	p->device = MTLCreateSystemDefaultDevice();
	if (not p->device)
		throw std::runtime_error("no Metal device");
	NSError * err = nil;
	id<MTLLibrary> lib = [p->device newLibraryWithSource:[NSString stringWithUTF8String:kernel_src] options:nil error:&err];
	if (not lib)
		throw std::runtime_error(std::string("pattern shader: ") + err.localizedDescription.UTF8String);
	p->pipeline = [p->device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"pattern"] error:&err];
	if (not p->pipeline)
		throw std::runtime_error(std::string("pattern pipeline: ") + err.localizedDescription.UTF8String);
	p->queue = [p->device newCommandQueue];
	if (CVMetalTextureCacheCreate(nullptr, nullptr, p->device, nullptr, &p->cache) != kCVReturnSuccess)
		throw std::runtime_error("CVMetalTextureCacheCreate failed");
}

TestPattern::~TestPattern()
{
	if (p->cache)
		CFRelease(p->cache);
}

void TestPattern::render(CVPixelBufferRef buf, const Pose & eye, const Fov & fov, uint64_t frame_index)
{
	size_t w = CVPixelBufferGetWidth(buf), h = CVPixelBufferGetHeight(buf);
	CVMetalTextureRef ty = nullptr, tc = nullptr;
	CVMetalTextureCacheCreateTextureFromImage(nullptr, p->cache, buf, nullptr, MTLPixelFormatR8Unorm, w, h, 0, &ty);
	CVMetalTextureCacheCreateTextureFromImage(nullptr, p->cache, buf, nullptr, MTLPixelFormatRG8Unorm, w / 2, h / 2, 1, &tc);
	if (not ty or not tc)
		throw std::runtime_error("cannot map pixel buffer planes to Metal (expects '420f')");

	params prm{
	        .q = {eye.orientation[0], eye.orientation[1], eye.orientation[2], eye.orientation[3]},
	        .tan_fov = {std::tan(fov.left), std::tan(fov.right), std::tan(fov.up), std::tan(fov.down)},
	        .frame = uint32_t(frame_index),
	        .bit_block = bit_block,
	        .bits = bits,
	};
	id<MTLCommandBuffer> cb = [p->queue commandBuffer];
	id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
	[enc setComputePipelineState:p->pipeline];
	[enc setTexture:CVMetalTextureGetTexture(ty) atIndex:0];
	[enc setTexture:CVMetalTextureGetTexture(tc) atIndex:1];
	[enc setBytes:&prm length:sizeof(prm) atIndex:0];
	[enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
	[enc endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	CFRelease(ty);
	CFRelease(tc);
}

int64_t TestPattern::read_frame_index(CVPixelBufferRef buf)
{
	if (CVPixelBufferGetPlaneCount(buf) < 1 or CVPixelBufferGetWidth(buf) < size_t(bit_block * bits))
		return -1;
	CVPixelBufferLockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);
	auto base = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(buf, 0);
	size_t stride = CVPixelBufferGetBytesPerRowOfPlane(buf, 0);
	int64_t v = 0;
	for (int b = 0; b < bits; ++b)
	{
		// sample the centre of the block
		uint8_t y = base[stride * (bit_block / 2) + b * bit_block + bit_block / 2];
		if (y > 192)
			v |= int64_t(1) << b;
		else if (y > 64)
		{
			v = -1;
			break;
		}
	}
	CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);
	return v;
}
} // namespace vrhost
