// SPDX-License-Identifier: GPL-3.0-or-later
// Per-eye Metal compositor: foveation (WiVRn's algorithm and parameters) and RGB -> YCbCr 4:2:0 into the
// encoder's '420f' (NV12 full range) CVPixelBuffer. The kernel is a Metal Shading Language port of WiVRn
// v26.9 server/compositor/shaders/foveation.comp (GPL-3.0-or-later; Collabora BSL-1.0 parts): same source
// ranges, same sRGB encode and BT.709 matrix. The chroma 2x2 average uses threadgroup memory instead of
// subgroup shuffles (identical result: WiVRn's 8x8 lane swizzle groups 2x2 quads).
#pragma once
#include "foveation_params.h"
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <memory>

namespace vrhost
{
struct EyeSource
{
	IOSurfaceRef surface;  // eye image (vrlink swapchain image)
	uint32_t pixel_format; // MTLPixelFormat of the surface
	bool srgb;             // data gamma encoded: sampled through the _sRGB view (linear), as WiVRn samples
	uint32_t x, y, w, h;   // sub-rect
	bool flip_y;
};

// Machine-side image options (Settings > VR "Quality"): the game may render below the headset's size (render scale,
// WivrnConfig) and is upscaled here to `upscale_w x upscale_h` before foveation; sharpening in the foveation pass.
struct CompositorOptions
{
	enum class Upscaler
	{
		metalfx,  // MetalFX spatial scaler (MTLFXSpatialScaler)
		bilinear, // MPSImageBilinearScale
	} upscaler = Upscaler::metalfx;
	uint32_t upscale_w = 0, upscale_h = 0; // 0 = never upscale (eyes larger than this are not touched)
	float sharpness = 0;                   // 0..1, unsharp mask strength in the foveation pass
	// Decode every 8-bit RGBA/BGRA eye image as sRGB, also when the game's format did not say so (DXGI *_TYPELESS
	// swapchains that hold sRGB data): fixes a washed-out picture; wrong for a game that really submits linear data.
	bool force_srgb = false;
};

class EyeCompositor
{
public:
	// out_w/out_h: encoded eye size (the encoder's configured size)
	EyeCompositor(uint32_t out_w, uint32_t out_h, CompositorOptions = {});
	// The upscaler of the last compose(): "metalfx", "bilinear" or "" (none)
	const char * last_upscaler() const;
	~EyeCompositor();
	EyeCompositor(const EyeCompositor &) = delete;
	EyeCompositor & operator=(const EyeCompositor &) = delete;

	// Encodes both eyes and waits for the GPU (the encoder reads the buffers right after). out[i] must be
	// '420f' IOSurface-backed CVPixelBuffers of out_w x out_h (VtEncoder::pool()). Returns false on error.
	// params[i] receives the foveation parameters to send to the headset with the frame.
	bool compose(const EyeSource src[2], const EyeFov fov[2], const float eye_x[2], CVPixelBufferRef out[2],
	             FoveationParams params[2]);

	// GPU time of the last compose() in ns (MTLCommandBuffer GPUEndTime - GPUStartTime)
	int64_t last_gpu_ns() const;

private:
	struct impl;
	std::unique_ptr<impl> p;
};
} // namespace vrhost
