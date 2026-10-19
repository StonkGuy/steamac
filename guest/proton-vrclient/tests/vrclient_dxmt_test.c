/*
 * Test for patches/0001: a D3D11 texture that implements DXMT's IMTLD3D11ExportableTexture, submitted through
 * Proton's vrclient_x64.dll, reaches the Linux OpenVR runtime as TextureType_Metal with the Metal handle, the
 * completion event and the texture description; any other D3D11 texture still takes the DXVK path.
 *
 * The program is what a game's openvr_api.dll is to vrclient_x64.dll: it loads the dll, lets it initialise the
 * VR registry key the way Proton's steam.exe does (vrclient_init_registry), gets IVRClientCore_003 and
 * IVRCompositor_028 (the version VRChat's XR plugin asks for) and submits both eyes for a number of frames.
 *   vrclient_dxmt_test.exe [IVRCompositor_0NN [frames [manual-registry]]]
 * manual-registry writes the registry key itself instead of calling vrclient_init_registry, for a runtime that
 * does not serve the IVRCompositor_028 Proton's own initialisation asks for.
 * The textures are COM objects of this program; no Direct3D device is needed. tests/run.sh runs it under the
 * Wine of the Proton build with guest/vr's runtime and vr-fakehost, which checks every submit it receives.
 *
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 */
#define COBJMACROS
#define CINTERFACE
#include <initguid.h>
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <stdio.h>

DEFINE_GUID(IID_IMTLD3D11ExportableTexture, 0xad6245d1, 0x3a11, 0x4dd6, 0x87,0xb7, 0x7a,0x58,0x04,0x8b,0x55,0xd3);

static int checks, failures;
#define CHECK(cond, ...) \
    do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* OpenVR, as seen through vrclient_x64.dll: C++ interfaces, this in rcx */
struct iface { void **vtbl; };
struct texture { void *handle; int32_t type; int32_t color_space; };
struct bounds { float u_min, v_min, u_max, v_max; };
enum { TextureType_DirectX = 0, ColorSpace_Gamma = 1, VRApplication_Scene = 1 };
#define CALL(type, obj, slot) ((type)((struct iface *)(obj))->vtbl[slot])
typedef int (*core_init_fn)(void *, int, const char *);
typedef void (*core_cleanup_fn)(void *);
typedef void *(*core_get_iface_fn)(void *, const char *, int *);
typedef int (*wait_get_poses_fn)(void *, void *, uint32_t, void *, uint32_t);
typedef int (*submit_fn)(void *, int, const struct texture *, const struct bounds *, int);
typedef int (*submit_array_fn)(void *, int, const struct texture *, uint32_t, const struct bounds *, int);
enum { CORE_INIT = 0, CORE_CLEANUP = 1, CORE_GET_IFACE = 3, COMP_WAIT_GET_POSES = 2, COMP_SUBMIT = 5, COMP_SUBMIT_ARRAY = 6 };

/* a D3D11 texture; with exportable set it also is DXMT's IMTLD3D11ExportableTexture */
struct fake_exportable;
typedef struct fake_exportable_vtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(struct fake_exportable *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(struct fake_exportable *);
    ULONG (STDMETHODCALLTYPE *Release)(struct fake_exportable *);
    HRESULT (STDMETHODCALLTYPE *GetMetalTexture)(struct fake_exportable *, UINT64 *, UINT64 *, UINT64 *);
} fake_exportable_vtbl;
struct fake_exportable { const fake_exportable_vtbl *lpVtbl; };

struct fake_texture
{
    ID3D11Texture2D texture2d;
    struct fake_exportable exportable_iface;
    BOOL exportable;
    LONG refs;
    UINT64 handle, event, value;
    D3D11_TEXTURE2D_DESC desc;
    unsigned int exports, other_queries;
};

static struct fake_texture *from_texture2d(ID3D11Texture2D *iface) { return CONTAINING_RECORD(iface, struct fake_texture, texture2d); }
static struct fake_texture *from_exportable(struct fake_exportable *iface) { return CONTAINING_RECORD(iface, struct fake_texture, exportable_iface); }

static HRESULT STDMETHODCALLTYPE tex_QueryInterface(ID3D11Texture2D *iface, REFIID riid, void **out)
{
    struct fake_texture *t = from_texture2d(iface);
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ID3D11DeviceChild) ||
        IsEqualGUID(riid, &IID_ID3D11Resource) || IsEqualGUID(riid, &IID_ID3D11Texture2D))
        *out = &t->texture2d;
    else if (t->exportable && IsEqualGUID(riid, &IID_IMTLD3D11ExportableTexture))
        *out = &t->exportable_iface;
    else
    {
        if (!IsEqualGUID(riid, &IID_IMTLD3D11ExportableTexture)) t->other_queries++;
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&t->refs);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE tex_AddRef(ID3D11Texture2D *iface) { return InterlockedIncrement(&from_texture2d(iface)->refs); }
static ULONG STDMETHODCALLTYPE tex_Release(ID3D11Texture2D *iface) { return InterlockedDecrement(&from_texture2d(iface)->refs); }
static void STDMETHODCALLTYPE tex_GetDevice(ID3D11Texture2D *iface, ID3D11Device **device) { *device = NULL; }
static HRESULT STDMETHODCALLTYPE tex_GetPrivateData(ID3D11Texture2D *iface, REFGUID guid, UINT *size, void *data) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE tex_SetPrivateData(ID3D11Texture2D *iface, REFGUID guid, UINT size, const void *data) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE tex_SetPrivateDataInterface(ID3D11Texture2D *iface, REFGUID guid, const IUnknown *data) { return E_NOTIMPL; }
static void STDMETHODCALLTYPE tex_GetType(ID3D11Texture2D *iface, D3D11_RESOURCE_DIMENSION *type) { *type = D3D11_RESOURCE_DIMENSION_TEXTURE2D; }
static void STDMETHODCALLTYPE tex_SetEvictionPriority(ID3D11Texture2D *iface, UINT priority) {}
static UINT STDMETHODCALLTYPE tex_GetEvictionPriority(ID3D11Texture2D *iface) { return 0; }
static void STDMETHODCALLTYPE tex_GetDesc(ID3D11Texture2D *iface, D3D11_TEXTURE2D_DESC *desc) { *desc = from_texture2d(iface)->desc; }
static const ID3D11Texture2DVtbl texture2d_vtbl =
{
    tex_QueryInterface, tex_AddRef, tex_Release, tex_GetDevice, tex_GetPrivateData, tex_SetPrivateData,
    tex_SetPrivateDataInterface, tex_GetType, tex_SetEvictionPriority, tex_GetEvictionPriority, tex_GetDesc,
};

static HRESULT STDMETHODCALLTYPE exp_QueryInterface(struct fake_exportable *iface, REFIID riid, void **out)
{
    return tex_QueryInterface(&from_exportable(iface)->texture2d, riid, out);
}
static ULONG STDMETHODCALLTYPE exp_AddRef(struct fake_exportable *iface) { return InterlockedIncrement(&from_exportable(iface)->refs); }
static ULONG STDMETHODCALLTYPE exp_Release(struct fake_exportable *iface) { return InterlockedDecrement(&from_exportable(iface)->refs); }
static HRESULT STDMETHODCALLTYPE exp_GetMetalTexture(struct fake_exportable *iface, UINT64 *texture, UINT64 *event, UINT64 *value)
{
    struct fake_texture *t = from_exportable(iface);
    t->exports++;
    *texture = t->handle;
    *event = t->event;
    *value = ++t->value; /* every flush moves the completion value on */
    return S_OK;
}
static const fake_exportable_vtbl exportable_vtbl = { exp_QueryInterface, exp_AddRef, exp_Release, exp_GetMetalTexture };

static void init_texture(struct fake_texture *t, BOOL exportable, UINT64 handle, UINT array_size)
{
    memset(t, 0, sizeof(*t));
    t->texture2d.lpVtbl = (ID3D11Texture2DVtbl *)&texture2d_vtbl;
    t->exportable_iface.lpVtbl = &exportable_vtbl;
    t->exportable = exportable;
    t->refs = 1;
    t->handle = handle;
    t->event = 0x2000;
    t->desc.Width = 2016;
    t->desc.Height = 2240;
    t->desc.MipLevels = 1;
    t->desc.ArraySize = array_size;
    t->desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    t->desc.SampleDesc.Count = 1;
    t->desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
}

static DWORD wait_vr_state(void)
{
    DWORD value = 0, size, type, i;
    HKEY key;

    for (i = 0; i < 600; i++, Sleep(50))
    {
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Wine\\VR", 0, KEY_READ, &key)) continue;
        size = sizeof(value);
        if (RegQueryValueExA(key, "state", NULL, &type, (BYTE *)&value, &size)) value = 0;
        RegCloseKey(key);
        if (value) break;
    }
    return value;
}

int main(int argc, char **argv)
{
    void *(CDECL *client_core_factory)(const char *, int *);
    BOOL (CDECL *init_registry)(void);
    const char *compositor_version = argc > 1 ? argv[1] : "IVRCompositor_028";
    int frames = argc > 2 ? atoi(argv[2]) : 60, err = -1, i, ok = 0, ret;
    struct fake_texture left, right, array, foreign;
    struct bounds whole = {0, 0, 1, 1}, flipped_half = {0, 1, 0.5f, 0};
    struct texture tex;
    void *core, *compositor;
    static char poses[64 * 128];
    BOOL manual_registry = argc > 3 && !strcmp(argv[3], "manual-registry");
    /* IVRCompositor_029 has GetSubmitTexture before Submit */
    int shift = strcmp(compositor_version, "IVRCompositor_029") >= 0;
    HMODULE vrclient;
    DWORD state;
    HKEY key;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!(vrclient = LoadLibraryA("vrclient_x64.dll")))
    {
        printf("FAIL: cannot load vrclient_x64.dll, error %lu\n", GetLastError());
        return 2;
    }
    init_registry = (void *)GetProcAddress(vrclient, "vrclient_init_registry");
    client_core_factory = (void *)GetProcAddress(vrclient, "VRClientCoreFactory");
    if (!init_registry || !client_core_factory) { printf("FAIL: vrclient_x64.dll exports\n"); return 2; }

    /* Proton's steam.exe does this once per session; games wait for state 1 */
    if (manual_registry)
    {
        state = 1;
        RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Wine\\VR", 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &key, NULL);
        RegSetValueExA(key, "openvr_vulkan_instance_extensions", 0, REG_SZ, (const BYTE *)"", 1);
        RegSetValueExA(key, "state", 0, REG_DWORD, (const BYTE *)&state, sizeof(state));
        RegCloseKey(key);
    }
    else CHECK(init_registry(), "vrclient_init_registry");
    state = wait_vr_state();
    CHECK(state == 1, "HKCU\\Software\\Wine\\VR state %ld, expected 1 (the runtime must serve Proton's own "
          "IVRClientCore_003 / IVRCompositor_028 calls)", (long)state);
    if (state != 1) goto done;

    core = client_core_factory("IVRClientCore_003", &err);
    CHECK(core != NULL, "IVRClientCore_003, error %d", err);
    if (!core) goto done;
    ret = CALL(core_init_fn, core, CORE_INIT)(core, VRApplication_Scene, NULL);
    CHECK(!ret, "IVRClientCore::Init error %d", ret);
    compositor = CALL(core_get_iface_fn, core, CORE_GET_IFACE)(core, compositor_version, &err);
    CHECK(compositor != NULL, "%s, error %d", compositor_version, err);
    if (!compositor) goto done;

    init_texture(&left, TRUE, 0x1000, 1);
    init_texture(&right, TRUE, 0x1001, 1);
    for (i = 0; i < frames; i++)
    {
        ret = CALL(wait_get_poses_fn, compositor, COMP_WAIT_GET_POSES)(compositor, poses, 64, NULL, 0);
        if (ret) { CHECK(0, "WaitGetPoses error %d at frame %d", ret, i); break; }
        tex.handle = &left.texture2d, tex.type = TextureType_DirectX, tex.color_space = ColorSpace_Gamma;
        ret = CALL(submit_fn, compositor, COMP_SUBMIT + shift)(compositor, 0, &tex, &whole, 0);
        tex.handle = &right.texture2d;
        ret |= CALL(submit_fn, compositor, COMP_SUBMIT + shift)(compositor, 1, &tex, &flipped_half, 0);
        if (!ret) ok++;
    }
    CHECK(ok == frames, "submits ok on %d/%d frames", ok, frames);
    CHECK(left.exports == (unsigned)frames && right.exports == (unsigned)frames, "GetMetalTexture calls %u/%u, expected %d each",
          left.exports, right.exports, frames);
    CHECK(left.refs == 1 && right.refs == 1, "texture references leaked: %ld/%ld", (long)left.refs, (long)right.refs);
    CHECK(!left.other_queries, "an exportable texture must not be offered to the DXVK path");

    /* one two-slice array texture for both eyes (single-pass stereo) */
    init_texture(&array, TRUE, 0x1100, 2);
    CALL(wait_get_poses_fn, compositor, COMP_WAIT_GET_POSES)(compositor, poses, 64, NULL, 0);
    tex.handle = &array.texture2d;
    ret = CALL(submit_array_fn, compositor, COMP_SUBMIT_ARRAY + shift)(compositor, 0, &tex, 1, &whole, 0);
    ret |= CALL(submit_array_fn, compositor, COMP_SUBMIT_ARRAY + shift)(compositor, 1, &tex, 0, &whole, 0);
    CHECK(!ret, "SubmitWithArrayIndex error %d", ret);
    CHECK(array.exports == 2 && array.refs == 1, "array texture exports %u refs %ld", array.exports, (long)array.refs);

    /* a D3D11 texture from another implementation: offered to the DXVK path as before, refused by this runtime */
    init_texture(&foreign, FALSE, 0x1200, 1);
    CALL(wait_get_poses_fn, compositor, COMP_WAIT_GET_POSES)(compositor, poses, 64, NULL, 0);
    tex.handle = &foreign.texture2d;
    ret = CALL(submit_fn, compositor, COMP_SUBMIT + shift)(compositor, 0, &tex, &whole, 0);
    CHECK(ret != 0, "a non-DXMT D3D11 texture was accepted");
    CHECK(!foreign.exports && foreign.other_queries >= 1 && foreign.refs == 1, "non-DXMT texture: exports %u, DXVK queries %u, refs %ld",
          foreign.exports, foreign.other_queries, (long)foreign.refs);

    CALL(core_cleanup_fn, core, CORE_CLEANUP)(core);
done:
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
