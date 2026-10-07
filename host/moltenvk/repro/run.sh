#!/bin/sh
# Host reproductions of guest workloads on a built host Vulkan driver (no VM needed).
#
#   host/moltenvk/repro/run.sh [libdir]                   MoltenVK: libMoltenVK.dylib in [libdir]
#                                                         (default work/out/host/lib), linked directly
#   REPRO_DRIVER=kosmickrisp host/moltenvk/repro/run.sh <libvulkan_kosmickrisp.dylib>
#                                                         KosmicKrisp through the Khronos loader
#                                                         (Homebrew vulkan-loader, test only: steamac
#                                                         opens the driver without a loader)
# 1. gamescope_cs.c: fetches gamescope 3.16.28 (pinned), compiles src/shaders/cs_*.comp with
#    glslang like gamescope's meson build (glslangValidator -V) and creates every pipeline
#    variant; cs_composite_blit must write the expected pixels from s_samplers[0] and from
#    the Y'CbCr (NV12) array s_ycbcr_samplers[0]. Metal argument buffers stay on (default). Run twice:
#    as gamescope creates its device, and with robustBufferAccess + robustBufferAccess2 (bounds-checked
#    MSL; the packed mat3x4 u_ctm[] select of the composite shaders failed to compile).
# 2. geometry.c: shaders/ (zink-style passthrough geometry shader with gl_PrimitiveIDIn,
#    list and strip draws, an array output varying: one mesh vertex member per element; DXVK-style
#    value-returning helpers, also nested helpers emitting vertices; vertex shaders with inactive or
#    duplicate builtin inputs: the object wrapper must match the wrapped vertex function's arguments) and
#    draws/dispatches with a VK_NULL_HANDLE pipeline bound (what Venus replays when host pipeline
#    creation failed).
# 3. depth_stencil.c: depth/stencil images with the usages zink gives GL renderbuffers (with
#    and without HOST_TRANSFER): every accepted usage must be allocatable and read back the
#    cleared depth/stencil values.
# 4. linear_pitch.c: LINEAR image with VkImageDrmFormatModifierExplicitCreateInfoEXT rowPitch
#    (virglrenderer dma-buf imports): layouts, memory size and pixel data use that pitch.
# 5. xfb.c: transform feedback captured by geometry shaders (DXVK stream-output style without
#    position and with rasterizer discard, strips with varying vertex counts, lines, 2 buffers,
#    buffer offsets/sizes, counter buffers), buffer contents checked; transform feedback queries
#    (primitives written/needed, overflow, other streams, vkd3d-proton and Venus result copies).
# 6. zero_init.c: compute shaders with zero-initialized workgroup memory (literal and
#    specialization-constant workgroup sizes), read back after a dispatch dirtied the memory.
# 7. free_after_signal.c: memory freed after a timeline semaphore signalled while the command buffer
#    that signalled it still runs (DXVK; Heroes Olden Era device loss).
# 8. robust_access.c: robustBufferAccess2 MSL (texel buffer atomic store, struct/packed matrix/array
#    loads, read-modify-write, runtime array after a header) with limited buffer ranges: in-bounds data,
#    out-of-bounds zeros, out-of-bounds stores discarded; a store through an OpCopyObject of an access chain
#    into a function-local array (dxil-spirv), values read back.
# 9. invalid_usage.c: VK_NULL_HANDLE set layouts in a pipeline layout (independent sets, from Venus) and
#    rasterizationSamples 8 (not supported by Apple GPUs); then a pipeline whose MSL does not compile, whose
#    MSL must be logged as "[mvk-msl] " lines on stderr.
# 10. vertex_input.c: vertex input layouts Metal's always-on vertex descriptor validation aborts on (zero
#    strides, static/dynamic/per instance/zero divisor, attributes past the stride, attributes of undescribed
#    bindings, with and without geometry shader emulation), one process per case; points check the
#    elements read.
# 11. device_address.c: atomics on vector components behind buffer device addresses (vkd3d-proton's
#    scalar-layout uvec3 counter, std430 uvec4 with a dynamic component, ivec2), values read back.
# 12. queries.c: occlusion query results copied from a later command buffer, one copy per query with
#    availability and wait (Venus' query feedback): availability of queries other than 0.
# 13. texel_buffer.c: texel buffer views at offsets that are not 16-byte aligned (single texel alignment, required by
#    vkd3d-proton): uniform/storage texel buffers, arrays, variable-count arrays, copies and push descriptors, values
#    read back; storage buffer array sizes written one element per update.
# 14. multi_entry.c: modules with vertex, fragment and compute entry points (shaders/multi_entry/ linked with
#    spirv-link, SPIR-V 1.0 and 1.6) whose compute entry point has workgroup variables (one zero-initialized):
#    the vertex/fragment pipeline draws, the compute pipeline counts in workgroup memory.
# 15. wgsize.c: threadgroup size of compute shaders with LocalSize / LocalSizeId (constants, specialization
#    constant) and zero-initialized workgroup memory (DXVK's new compiler: LocalSizeId + OpConstantNull ran
#    1x1x1 threadgroups); every invocation and the shared counter of each workgroup checked.
# 16. descriptor_heap.c: vkd3d-proton's descriptor heaps without descriptor buffers / mutable descriptors (one
#    variable-count update-after-bind set per descriptor type, a pool per heap sized exactly for its sets), heaps
#    of 1 to 1000000 views and 1 to 2048 samplers, shader-visible and host, and pools of one such set: every set
#    allocates and is written null (variable sets with buffer sizes counted the size of each element twice, and
#    texel offsets made it worse: Stellar Blade's first heap failed with VK_ERROR_OUT_OF_POOL_MEMORY and Venus
#    stopped the game's command stream); descriptors at element 999997 read back (dh_read.comp), the raw SSBO
#    through two declarations of its binding, restrict and not (the alias's cast dropped __restrict); a graphics
#    pipeline whose fragment shader loads heap descriptors in a loop header block (shaders/heap/, Stellar Blade's
#    shape: SPIRV-Cross declared their access chains as temporaries that Metal rejects); memory per pipeline
#    over 16 specialized variants using the 1000000-descriptor heaps (< 4 MB: Metal kept a 32 MB table per
#    inline array<T, 1000000> and program, ~100 MB per pipeline).
# 17. msl_helpers.c: fragment helpers with an OpKill-only block, directly and through a nested caller
#    (STEAMAC-1Q): discard left-half pixels, render right-half colors, suppress writes after discard;
#    a compute helper named log10(float) (STEAMAC-1R), its implementation's values read back.
# 18. device_lost.c (KosmicKrisp only): a device loss must be printed on stderr (STEAMAC-G: a release build lost
#    the device silently and Venus only logged "vkQueueSubmit resulted in CS error"); the runtime's queue-loss
#    path (a timeline signal of value 0) stands in for a failed Metal command buffer, which can't be provoked.
# 19. host_memory.c: 16 KiB host-visible allocations as virglrenderer makes them (POSIX shm imported as host memory,
#    the shm fd plus two dups per mapped allocation; STEAMAC-G: Left 4 Dead 2's context died after a blob export and
#    shm_open failed): at a Finder launch's soft RLIMIT_NOFILE of 256 they run out of fds within 256 / 3 allocations,
#    the driver never failing; at the limit steamac-vm raises itself to, 8192 live ones, 20000 free + allocate rounds and
#    8192 live plain allocations of that type succeed, and every fd is closed after the frees.
# 20. sparse.c (KosmicKrisp only): vkd3d-proton's Tiled Resources Tier 2 feature gate, standard 2D tiles and
#    per-layer mip tails, strict unbound image/buffer reads and shader residency, minimum-LOD clamps,
#    binary/timeline sparse bind ordering, page relocation and image/plain-buffer aliases; vkd3d-proton's remap stress
#    (100 rounds of tile range updates batched like its UpdateTileMappings: NULL ranges ending a vkQueueBindSparse must
#    unmap); texel buffer views of sparse buffers made before and after binds, following rebinds and unmaps, and the
#    residency of OpImageSparseRead/OpImageSparseFetch through them (CheckAccessFullyMapped after typed buffer loads,
#    4- and 16-byte texels); residency of explicit-gradient, explicit-LOD and fragment implicit-LOD samples reaching or
#    passing the last level (NEAREST and
#    LINEAR mip); R32/R8 sampler MIN/MAX reduction over bilinear footprints and adjacent mip levels, with
#    weighted-average controls.
#    imageLoad residency through single-level storage views of levels > 0 is printed as KNOWN, not failed: Metal
#    reports residency of a view with baseMipLevel b for the image's level lod instead of b + lod (values are right).
# All run with Metal API validation in assert mode (MTL_DEBUG_LAYER), so a Metal validation error
# fails the run instead of aborting a VM later.
# All applicable tests must pass on MoltenVK. On KosmicKrisp all but 5 and 10 must pass (1 sizes the descriptor pool with
# combinedImageSamplerDescriptorCount like gamescope; 2 skips the VK_NULL_HANDLE binds, which virglrenderer no
# longer passes to the driver; 9 without the MSL log, which is MoltenVK's; 13 expects (0, 0, 0, 0) past the end of
# an RGBA8 view, MoltenVK returns alpha 1); not run there: 5 (Mesa !44928 is a draft: strip-GS transform feedback
# pipelines fail to create, the counter overshoots on overflow), 10 (expects MoltenVK's clean creation failures;
# KosmicKrisp creates and draws these layouts).
set -eu

GAMESCOPE_REPO=https://github.com/ValveSoftware/gamescope.git
GAMESCOPE_TAG=3.16.28
GAMESCOPE_COMMIT=fa0b4d3342078f01eadff0193e09c3b561f40c03

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
inc=$root/work/build/host-moltenvk/src/Package/Release/MoltenVK/include
driver=${REPRO_DRIVER:-moltenvk}
case $driver in
moltenvk)
	work=$root/work/build/host-moltenvk/repro
	libdir=${1:-$root/work/out/host/lib}
	link="-L$libdir -lMoltenVK -Wl,-rpath,$libdir"
	export MVK_CONFIG_LOG_LEVEL=1
	;;
kosmickrisp)
	work=$root/work/build/host-kosmickrisp/repro
	kk=${1:?usage: REPRO_DRIVER=kosmickrisp $0 <libvulkan_kosmickrisp.dylib>}
	brew list --versions vulkan-loader > /dev/null 2>&1 || brew install vulkan-loader
	loader=$(brew --prefix vulkan-loader)/lib
	link="-L$loader -lvulkan -Wl,-rpath,$loader"
	mkdir -p "$work"
	printf '{"file_format_version": "1.0.1", "ICD": {"library_path": "%s", "api_version": "1.4.0"}}\n' \
		"$kk" > "$work/kosmickrisp_icd.json"
	export VK_DRIVER_FILES="$work/kosmickrisp_icd.json"
	unset VK_ICD_FILENAMES
	;;
*)
	echo "REPRO_DRIVER must be moltenvk or kosmickrisp" >&2
	exit 2
	;;
esac
export MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=assert

brew list --versions glslang > /dev/null 2>&1 || brew install glslang
brew list --versions spirv-tools > /dev/null 2>&1 || brew install spirv-tools
mkdir -p "$work"

# build <program> [cflags...]: $here/<program>.c -> $work/<program>, linked against the driver
build() {
	prog=$1
	shift
	# shellcheck disable=SC2086
	xcrun clang -std=c11 -Wall -Werror -O1 "$@" -I"$inc" "$here/$prog.c" $link -o "$work/$prog"
}

src=$work/gamescope
if [ ! -d "$src/.git" ]; then
	git init -q "$src"
	git -C "$src" remote add origin "$GAMESCOPE_REPO"
fi
if ! git -C "$src" cat-file -e "$GAMESCOPE_COMMIT^{commit}" 2> /dev/null; then
	git -C "$src" fetch -q --depth 1 origin "refs/tags/$GAMESCOPE_TAG:refs/tags/$GAMESCOPE_TAG"
fi
test "$(git -C "$src" rev-parse "$GAMESCOPE_TAG^{commit}")" = "$GAMESCOPE_COMMIT"
git -C "$src" checkout -q -f --detach "$GAMESCOPE_COMMIT"

spv=$work/spv
rm -rf "$spv"
mkdir -p "$spv"
for s in "$src"/src/shaders/cs_*.comp; do
	glslangValidator -V --quiet "$s" -o "$spv/$(basename "$s" .comp).spv"
done

build gamescope_cs
"$work/gamescope_cs" "$spv"
"$work/gamescope_cs" "$spv" robust2

gspv=$work/geometry-spv
rm -rf "$gspv"
mkdir -p "$gspv"
for s in "$here"/shaders/*.vert "$here"/shaders/*.geom "$here"/shaders/*.frag; do
	glslangValidator -V --quiet "$s" -o "$gspv/$(basename "$s").spv"
done
for s in "$here"/shaders/*.comp; do
	glslangValidator -V --quiet --target-env vulkan1.3 "$s" -o "$gspv/$(basename "$s").spv"
done
for s in "$here"/shaders/*.spvasm; do
	spirv-as --target-env vulkan1.0 "$s" -o "$gspv/$(basename "$s" .spvasm).spv"
done
build geometry
"$work/geometry" "$gspv"

build depth_stencil
"$work/depth_stencil"

build linear_pitch
"$work/linear_pitch"

if [ "$driver" = moltenvk ]; then
	build xfb
	"$work/xfb" "$gspv"
fi

build zero_init
"$work/zero_init" "$gspv"

glslangValidator -V --quiet -x "$here/shaders/busy.comp" -o "$work/busy.comp.inc"
build free_after_signal -I"$work"
"$work/free_after_signal"

build robust_access
"$work/robust_access" "$gspv"

build invalid_usage
"$work/invalid_usage" "$gspv"
if [ "$driver" = moltenvk ]; then
	# The failing MSL is logged after the error: the first lines without SPIRV-Cross' helper templates, and the
	# lines around the error locations, each once (the two errors are on neighboring lines).
	msl_log=$work/msl-log.txt
	"$work/invalid_usage" "$gspv" msl-log 2> "$msl_log"
	msl_context=$(grep '^\[mvk-msl\] [0-9][0-9]*: ' "$msl_log" || true)
	if grep -q '^\[mvk-msl\] #include <metal_stdlib>$' "$msl_log" && grep -q '^\[mvk-msl\] \.\.\. ([0-9]* lines of templates left out)$' "$msl_log" &&
		! grep -q '^\[mvk-msl\] struct spvUnsafeArray' "$msl_log" && [ "$(printf '%s\n' "$msl_context" | grep -c 'double')" -eq 2 ] &&
		[ -z "$(printf '%s\n' "$msl_context" | sed 's/: .*//' | sort | uniq -d)" ]; then
		echo "OK   failing MSL logged: $(grep -c '^\[mvk-msl\] ' "$msl_log") [mvk-msl] lines, $(grep '^\[mvk-msl\] \.\.\. (' "$msl_log" | sed 's/^\[mvk-msl\] //' | tr '\n' ' ')error context (each line once):"
		printf '%s\n' "$msl_context"
	else
		echo "FAIL failing MSL not logged as [mvk-msl] lines (head without templates, both error lines, each context line once):"
		cat "$msl_log"
		exit 1
	fi

	build vertex_input
	"$work/vertex_input" "$gspv"
fi

build device_address
"$work/device_address" "$gspv"

build queries
"$work/queries" "$gspv"

build texel_buffer
"$work/texel_buffer" "$gspv"

mspv=$work/multi-entry-spv
rm -rf "$mspv"
mkdir -p "$mspv"
for env in vulkan1.0 vulkan1.3; do
	glslangValidator -V --quiet --target-env $env -e vs_main --source-entrypoint main "$here/shaders/multi_entry/vs.vert" -o "$mspv/vs.$env.spv"
	glslangValidator -V --quiet --target-env $env -e fs_main --source-entrypoint main "$here/shaders/multi_entry/fs.frag" -o "$mspv/fs.$env.spv"
	glslangValidator -V --quiet --target-env $env -e cs_main --source-entrypoint main "$here/shaders/multi_entry/cs.comp" -o "$mspv/cs.$env.spv"
	spirv-link --target-env $env "$mspv/vs.$env.spv" "$mspv/fs.$env.spv" "$mspv/cs.$env.spv" -o "$mspv/multi_entry.$env.spv"
done
build multi_entry
"$work/multi_entry" "$mspv"

wspv=$work/wgsize-spv
rm -rf "$wspv"
mkdir -p "$wspv"
for s in "$here"/shaders/wgsize/*.spvasm; do
	spirv-as --target-env vulkan1.3 "$s" -o "$wspv/$(basename "$s" .spvasm).spv"
done
build wgsize
"$work/wgsize" "$wspv"

spirv-as --target-env vulkan1.3 "$here/shaders/heap/dh_loop_header.spvasm" -o "$gspv/dh_loop_header.spv"
build descriptor_heap
"$work/descriptor_heap" "$gspv"

build msl_helpers
"$work/msl_helpers" "$gspv"

if [ "$driver" = kosmickrisp ]; then
	build device_lost
	"$work/device_lost"
fi

build host_memory
"$work/host_memory"

if [ "$driver" = kosmickrisp ]; then
	build sparse
	"$work/sparse" "$gspv"
fi
