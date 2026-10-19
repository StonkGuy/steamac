// SPDX-License-Identifier: GPL-3.0-or-later
// See eye_compositor.h. Built with -fobjc-arc.
#include "eye_compositor.h"

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#include <cstring>
#include <stdexcept>

namespace vrhost
{
namespace
{
// MSL port of WiVRn foveation.comp (one eye per dispatch)
const char * kShader = R"MSL(
#include <metal_stdlib>
using namespace metal;

constant float3x3 color_space = float3x3(
	float3(0.2126, -0.1146, 0.5),
	float3(0.7152, -0.3854, -0.4542),
	float3(0.0722, 0.5, -0.0458)); // columns = R, G, B weights for (Y, Cb, Cr)

#define MAX_SAMPLES 4

static float to_srgb(float v) { return v < 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }

kernel void foveate(texture2d<float, access::read> source [[texture(0)]],
                    texture2d<float, access::write> luma [[texture(1)]],
                    texture2d<float, access::write> chroma [[texture(2)]],
                    device const uint *bx [[buffer(0)]],
                    device const uint *by [[buffer(1)]],
                    constant float &sharpness [[buffer(2)]],
                    uint2 gid [[thread_position_in_grid]],
                    uint2 lid [[thread_position_in_threadgroup]])
{
	threadgroup float2 cbcr[8][8];
	uint ix = gid.x, iy = gid.y;
	bool inside = ix < luma.get_width() && iy < luma.get_height();
	float3 ycc = float3(0, 0.5, 0.5);
	if (inside) {
		uint ymin = by[iy], ymax = by[iy + 1];
		if (ymin > ymax) { uint t = ymin; ymin = ymax; ymax = t; }
		uint xmin = bx[ix], xmax = bx[ix + 1];
		if (xmin > xmax) { uint t = xmin; xmin = xmax; xmax = t; }
		uint xstep = max((xmax - xmin + MAX_SAMPLES - 1) / MAX_SAMPLES, 1u);
		uint ystep = max((ymax - ymin + MAX_SAMPLES - 1) / MAX_SAMPLES, 1u);
		float4 colour = 0;
		uint count = 0;
		for (uint y = ymin; y < ymax; y += ystep)
			for (uint x = xmin; x < xmax; x += xstep) {
				colour += source.read(uint2(x, y));
				++count;
			}
		colour /= max(count, 1u);
		if (sharpness > 0) {
			// unsharp mask against the neighbouring source pixels just outside this output pixel's band
			int2 lim = int2(source.get_width() - 1, source.get_height() - 1);
			int xm = int(xmin + xmax) / 2, ym = int(ymin + ymax) / 2;
			float4 n = source.read(uint2(clamp(int2(int(xmin) - 1, ym), int2(0), lim)))
			         + source.read(uint2(clamp(int2(int(xmax), ym), int2(0), lim)))
			         + source.read(uint2(clamp(int2(xm, int(ymin) - 1), int2(0), lim)))
			         + source.read(uint2(clamp(int2(xm, int(ymax)), int2(0), lim)));
			colour = clamp(colour + sharpness * (colour - n / 4), 0.0, 1.0);
		}
		float3 rgb = float3(to_srgb(colour.r), to_srgb(colour.g), to_srgb(colour.b));
		ycc = color_space * rgb + float3(0, 0.5, 0.5);
		luma.write(float4(ycc.x), uint2(ix, iy));
	}
	cbcr[lid.y][lid.x] = ycc.yz;
	threadgroup_barrier(mem_flags::mem_threadgroup);
	if (inside && (lid.x % 2) == 0 && (lid.y % 2) == 0 && ix / 2 < chroma.get_width() && iy / 2 < chroma.get_height()) {
		float2 s = cbcr[lid.y][lid.x] + cbcr[lid.y][lid.x + 1] + cbcr[lid.y + 1][lid.x] + cbcr[lid.y + 1][lid.x + 1];
		chroma.write(float4(s / 4, 0, 0), uint2(ix / 2, iy / 2));
	}
}
)MSL";

MTLPixelFormat srgb_view(MTLPixelFormat f)
{
	switch (f)
	{
		case MTLPixelFormatRGBA8Unorm:
			return MTLPixelFormatRGBA8Unorm_sRGB;
		case MTLPixelFormatBGRA8Unorm:
			return MTLPixelFormatBGRA8Unorm_sRGB;
		default:
			return f;
	}
}
} // namespace

struct EyeCompositor::impl
{
	uint32_t out_w, out_h;
	id<MTLDevice> dev;
	id<MTLCommandQueue> queue;
	id<MTLComputePipelineState> pso;
	CVMetalTextureCacheRef cache = nullptr;
	id<MTLBuffer> bounds[2][2]; // [eye][x|y], out_dim + 1 uint32 each
	// parameter cache, like WiVRn update_ubo: recompute only when the inputs change
	struct Key
	{
		EyeFov fov;
		float eye_x;
		uint32_t x, y, w, h;
		bool flip;
		bool operator==(const Key &) const = default;
	} key[2]{};
	bool key_valid[2]{};
	FoveationParams params[2];
	int64_t gpu_ns = 0;
	CompositorOptions opt;
	// upscaling (render scale < 1): eye rect copied to crop[e], scaled into up[e]
	id<MTLTexture> crop[2], up[2];
	id<MTLFXSpatialScaler> fx;
	MPSImageBilinearScale * bilinear = nil;
	MTLPixelFormat crop_fmt = MTLPixelFormatInvalid;
	uint32_t crop_w = 0, crop_h = 0;
	const char * used = "";

	bool prepare_upscale(MTLPixelFormat fmt, uint32_t w, uint32_t h);
};

bool EyeCompositor::impl::prepare_upscale(MTLPixelFormat fmt, uint32_t w, uint32_t h)
{
	if (crop[0] && crop_fmt == fmt && crop_w == w && crop_h == h)
		return true;
	fx = nil;
	MTLTextureDescriptor * d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
	d.usage = MTLTextureUsageShaderRead;
	d.storageMode = MTLStorageModePrivate;
	MTLPixelFormat up_fmt = MTLPixelFormatRGBA16Float; // linear, read by the foveation pass
	MTLTextureUsage up_usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
	if (opt.upscaler == CompositorOptions::Upscaler::metalfx)
	{
		MTLFXSpatialScalerDescriptor * sd = [MTLFXSpatialScalerDescriptor new];
		sd.inputWidth = w;
		sd.inputHeight = h;
		sd.outputWidth = opt.upscale_w;
		sd.outputHeight = opt.upscale_h;
		sd.colorTextureFormat = fmt;
		sd.outputTextureFormat = up_fmt;
		sd.colorProcessingMode = MTLFXSpatialScalerColorProcessingModeLinear; // sRGB data is read through the sRGB view
		if ([MTLFXSpatialScalerDescriptor supportsDevice:dev])
			fx = [sd newSpatialScalerWithDevice:dev];
		if (fx)
		{
			d.usage |= fx.colorTextureUsage;
			up_usage |= fx.outputTextureUsage;
		}
	}
	if (!fx && !bilinear)
		bilinear = [[MPSImageBilinearScale alloc] initWithDevice:dev];
	MTLTextureDescriptor * u = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:up_fmt width:opt.upscale_w
	                                                                             height:opt.upscale_h mipmapped:NO];
	u.usage = up_usage;
	u.storageMode = MTLStorageModePrivate;
	for (int e = 0; e < 2; e++)
	{
		crop[e] = [dev newTextureWithDescriptor:d];
		up[e] = [dev newTextureWithDescriptor:u];
		if (!crop[e] || !up[e])
			return false;
	}
	crop_fmt = fmt;
	crop_w = w;
	crop_h = h;
	return true;
}

EyeCompositor::EyeCompositor(uint32_t out_w, uint32_t out_h, CompositorOptions opt) : p(std::make_unique<impl>())
{
	p->opt = opt;
	p->out_w = out_w;
	p->out_h = out_h;
	p->dev = MTLCreateSystemDefaultDevice();
	if (!p->dev)
		throw std::runtime_error("no Metal device");
	p->queue = [p->dev newCommandQueue];
	NSError * err = nil;
	id<MTLLibrary> lib = [p->dev newLibraryWithSource:@(kShader) options:nil error:&err];
	if (!lib)
		throw std::runtime_error(std::string("foveation shader: ") + err.localizedDescription.UTF8String);
	p->pso = [p->dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"foveate"] error:&err];
	if (!p->pso)
		throw std::runtime_error("foveation pipeline");
	if (CVMetalTextureCacheCreate(nullptr, nullptr, p->dev, nullptr, &p->cache) != kCVReturnSuccess)
		throw std::runtime_error("CVMetalTextureCache");
	for (auto & eye: p->bounds)
	{
		eye[0] = [p->dev newBufferWithLength:(out_w + 1) * 4 options:MTLResourceStorageModeShared];
		eye[1] = [p->dev newBufferWithLength:(out_h + 1) * 4 options:MTLResourceStorageModeShared];
	}
}

EyeCompositor::~EyeCompositor()
{
	if (p->cache)
		CFRelease(p->cache);
}

int64_t EyeCompositor::last_gpu_ns() const
{
	return p->gpu_ns;
}

const char * EyeCompositor::last_upscaler() const
{
	return p->used;
}

bool EyeCompositor::compose(const EyeSource src_in[2], const EyeFov fov[2], const float eye_x[2], CVPixelBufferRef out[2],
                            FoveationParams params[2])
{
	@autoreleasepool
	{
		id<MTLCommandBuffer> cb = [p->queue commandBuffer];
		CVMetalTextureRef planes[2][2] = {};
		id<MTLTexture> in[2];
		EyeSource src[2] = {src_in[0], src_in[1]};
		bool ok = true;
		p->used = "";
		for (int e = 0; e < 2 && ok; e++)
		{
			MTLPixelFormat fmt = src[e].srgb || p->opt.force_srgb ? srgb_view((MTLPixelFormat)src[e].pixel_format) : (MTLPixelFormat)src[e].pixel_format;
			MTLTextureDescriptor * d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
			                                                                              width:IOSurfaceGetWidth(src[e].surface)
			                                                                             height:IOSurfaceGetHeight(src[e].surface)
			                                                                          mipmapped:NO];
			d.usage = MTLTextureUsageShaderRead;
			in[e] = [p->dev newTextureWithDescriptor:d iosurface:src[e].surface plane:0];
			ok = in[e] != nil;
			// render scale below the headset's size: crop the eye rect, upscale it, foveate the upscaled image
			if (ok && p->opt.upscale_w > src[e].w && p->opt.upscale_h > src[e].h)
			{
				ok = p->prepare_upscale(fmt, src[e].w, src[e].h);
				if (!ok)
					break;
				id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
				[be copyFromTexture:in[e] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(src[e].x, src[e].y, 0)
				         sourceSize:MTLSizeMake(src[e].w, src[e].h, 1) toTexture:p->crop[e] destinationSlice:0
				   destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
				[be endEncoding];
				if (p->fx)
				{
					p->fx.colorTexture = p->crop[e];
					p->fx.outputTexture = p->up[e];
					p->fx.inputContentWidth = src[e].w;
					p->fx.inputContentHeight = src[e].h;
					[p->fx encodeToCommandBuffer:cb];
					p->used = "metalfx";
				}
				else
				{
					[p->bilinear encodeToCommandBuffer:cb sourceTexture:p->crop[e] destinationTexture:p->up[e]];
					p->used = "bilinear";
				}
				in[e] = p->up[e];
				src[e].x = src[e].y = 0;
				src[e].w = p->opt.upscale_w;
				src[e].h = p->opt.upscale_h;
			}
		}
		id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
		[ce setComputePipelineState:p->pso];
		float sharp = p->opt.sharpness;
		for (int e = 0; e < 2 && ok; e++)
		{
			impl::Key k{fov[e], eye_x[e], src[e].x, src[e].y, src[e].w, src[e].h, src[e].flip_y};
			if (!p->key_valid[e] || !(p->key[e] == k))
			{
				p->params[e] = compute_foveation(fov[e], eye_x[e], src[e].w, src[e].h, p->out_w, p->out_h);
				auto bx = foveation_boundaries(p->params[e].x, false, src[e].x, src[e].w, p->out_w);
				auto by = foveation_boundaries(p->params[e].y, src[e].flip_y, src[e].y, src[e].h, p->out_h);
				memcpy(p->bounds[e][0].contents, bx.data(), bx.size() * 4);
				memcpy(p->bounds[e][1].contents, by.data(), by.size() * 4);
				p->key[e] = k;
				p->key_valid[e] = true;
			}
			params[e] = p->params[e];
			for (int pl = 0; pl < 2 && ok; pl++)
				ok = CVMetalTextureCacheCreateTextureFromImage(nullptr, p->cache, out[e], nullptr,
				                                               pl ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm,
				                                               CVPixelBufferGetWidthOfPlane(out[e], pl),
				                                               CVPixelBufferGetHeightOfPlane(out[e], pl), pl,
				                                               &planes[e][pl]) == kCVReturnSuccess;
			if (!ok)
				break;
			[ce setTexture:in[e] atIndex:0];
			[ce setTexture:CVMetalTextureGetTexture(planes[e][0]) atIndex:1];
			[ce setTexture:CVMetalTextureGetTexture(planes[e][1]) atIndex:2];
			[ce setBuffer:p->bounds[e][0] offset:0 atIndex:0];
			[ce setBuffer:p->bounds[e][1] offset:0 atIndex:1];
			[ce setBytes:&sharp length:sizeof sharp atIndex:2];
			[ce dispatchThreadgroups:MTLSizeMake((p->out_w + 7) / 8, (p->out_h + 7) / 8, 1)
			   threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
		}
		[ce endEncoding];
		if (ok)
		{
			[cb commit];
			[cb waitUntilCompleted];
			ok = cb.status == MTLCommandBufferStatusCompleted;
			p->gpu_ns = int64_t((cb.GPUEndTime - cb.GPUStartTime) * 1e9);
		}
		for (auto & e: planes)
			for (auto t: e)
				if (t)
					CFRelease(t);
		CVMetalTextureCacheFlush(p->cache, 0);
		return ok;
	}
}
} // namespace vrhost
