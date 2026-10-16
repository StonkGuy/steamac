# d3dr — Direct3D 11 remoting to D3DMetal on the Mac (experimental)

d3dr lets a Direct3D 11 program in the steamac guest draw with Apple's D3DMetal on the Mac. D3DMetal (part of
Apple's Game Porting Toolkit) only runs inside a CrossOver-derived macOS Wine process: its Windows DLLs hand out
COM objects implemented by an x86_64 macOS framework in the same address space, so there is no narrow interface
that could be forwarded the way DXMT's `winemetal` calls are. d3dr therefore remotes the public D3D11 API itself:

```
guest (Proton under FEX)                      Mac
game -> d3dr d3d11.dll --TCP (gvproxy)--> d3dr-server.exe (user's macOS Wine) -> D3DMetal -> Metal
```

* `src/client.cpp` — the guest `d3d11.dll`. Objects are client-side shells with client-assigned handles;
  calls are encoded into a stream (`include/d3dr_proto.h`) and sent in batches. Only device creation, `Map`
  for reading and `Present` pacing (at most two frames in flight) wait for the server.
* `src/server.cpp` — `d3dr-server.exe`, replays the stream against the D3D11 of the Wine it runs in and presents
  into its own window.
* Nothing of Apple's is part of steamac or of this build. The user installs D3DMetal (GPTK, or a Sikarugir
  template) and a CrossOver-derived Wine engine; `tools/d3dr-host.sh` runs the server with them in place.
  D3DMetal's licence (GPTK EULA) restricts it to Apple hardware and to developing, testing or evaluating games;
  d3dr uses only the documented D3D11 API and never modifies or inspects D3DMetal.

## Status (M1)

The D3D11 subset that `d3d11bench` and `tests/d3dr_triangle.cpp` use: buffers, 2D textures, views, shaders,
input layouts, state objects, the immediate context's IA/VS/PS/RS/OM state and draws, `Map`/`Unmap`,
`UpdateSubresource`, `CopyResource`, `D3D11CreateDeviceAndSwapChain` with `GetBuffer(0)`/`Present`. Every other
method logs `d3dr: unimplemented ...` once and fails (the stub bases are generated from the mingw-w64 headers,
`tools/gen_stubs.py`). No DXGI factory/adapters, deferred contexts, queries, resize, or D3D12 yet; the game's
own window stays empty (the picture is in the server's window on the Mac). Dynamic data is copied whole at each
`Unmap`.

## Build and test

```
make -C d3dr            # build/d3d11.dll, d3dr-server.exe, d3dr_triangle.exe, d3d11bench.exe (llvm-mingw: MINGW=)
make -C d3dr test       # protocol unit test (native)
D3DR_WINE=<engine> D3DR_D3DMETAL=<renderer/d3dmetal> WINEPREFIX=<scratch> d3dr/tests/loopback.sh
```

`loopback.sh` runs the triangle test on the Mac twice: directly on D3DMetal (the oracle) and through d3dr; both
must read back the same pixels. `BENCH=1` adds d3d11bench both ways.

In the guest: start the server on the Mac (`d3dr/tools/d3dr-host.sh`), copy `build/d3d11.dll` and a test program
to `~/vrc/d3dr/`, then `d3dr/guest/d3dr-run.sh ~/vrc/d3dr/d3dr_triangle.exe`.
