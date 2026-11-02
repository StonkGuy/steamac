// Standalone check of work/out/host/lib/libvirglrenderer.1.dylib with the flags libkrun
// uses on macOS: Venus renderer init (render server thread, MoltenVK), the Venus capset,
// a Venus context, and a mappable host blob exported as a shared-memory fd (what libkrun
// hv_vm_maps into the guest). Exits non-zero on any failure.
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <virgl/virglrenderer.h>

#define VIRGL_RENDERER_CAPSET_VENUS 4

static void write_fence(void *cookie, uint32_t fence)
{
    (void)cookie;
    (void)fence;
}

static struct virgl_renderer_callbacks cbs = {
    .version = 3,
    .write_fence = write_fence,
};

static int failures;

static void check(const char *what, long got, long want)
{
    printf("%-36s = %ld%s\n", what, got, got == want ? "" : "  <-- FAIL");
    if (got != want)
        failures++;
}

int main(void)
{
    // A flags override can accidentally request GL with Venus-only callbacks.
    // Reject this before vrend's context wrapper calls a NULL client function.
    check("GL without context callbacks",
          virgl_renderer_init(&failures, VIRGL_RENDERER_VENUS, &cbs), -EINVAL);
    virgl_renderer_cleanup(&failures);

    int flags = VIRGL_RENDERER_VENUS | VIRGL_RENDERER_NO_VIRGL | VIRGL_RENDERER_RENDER_SERVER |
                VIRGL_RENDERER_ASYNC_FENCE_CB;
    int ret = virgl_renderer_init(NULL, flags, &cbs);
    check("virgl_renderer_init", ret, 0);
    if (ret)
        return 1;

    uint32_t max_ver = 0, size = 0;
    virgl_renderer_get_cap_set(VIRGL_RENDERER_CAPSET_VENUS, &max_ver, &size);
    uint32_t caps[64] = {0};
    if (size <= sizeof(caps))
        virgl_renderer_fill_caps(VIRGL_RENDERER_CAPSET_VENUS, 0, caps);
    printf("venus capset: %u bytes, wire format %u, vk.xml %u.%u.%u\n", size, caps[0],
           caps[1] >> 22, (caps[1] >> 12) & 0x3ff, caps[1] & 0xfff);
    if (size == 0 || caps[0] == 0)
        failures++;

    check("virgl_renderer_context_create_with_flags",
          virgl_renderer_context_create_with_flags(1, VIRGL_RENDERER_CAPSET_VENUS, 5, "check"), 0);

    struct virgl_renderer_resource_create_blob_args blob = {
        .res_handle = 1,
        .ctx_id = 1,
        .blob_mem = VIRGL_RENDERER_BLOB_MEM_HOST3D,
        .blob_flags = VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE,
        .size = 65536,
    };
    check("virgl_renderer_resource_create_blob", virgl_renderer_resource_create_blob(&blob), 0);

    uint32_t fd_type = 0;
    int fd = -1;
    check("virgl_renderer_resource_export_blob",
          virgl_renderer_resource_export_blob(1, &fd_type, &fd), 0);
    check("  fd type (3 = SHM)", fd_type, VIRGL_RENDERER_BLOB_FD_TYPE_SHM);
    check("  fd valid", fd >= 0, 1);

    uint32_t map_info = 0;
    check("virgl_renderer_resource_get_map_info", virgl_renderer_resource_get_map_info(1, &map_info),
          0);
    printf("  map_info 0x%x\n", map_info);

    printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
