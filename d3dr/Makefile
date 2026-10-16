# d3dr: Direct3D 11 remoting (guest d3d11.dll -> d3dr-server.exe on the Mac, which runs the user's D3DMetal).
#   make            build/d3d11.dll (client), build/d3dr-server.exe, build/d3dr_triangle.exe, build/d3d11bench.exe
#   make test       native protocol unit test (build/proto_test)
#   tests/loopback.sh   end-to-end on the Mac: triangle + d3d11bench direct on D3DMetal vs through d3dr
# MINGW = an llvm-mingw toolchain (x86_64-w64-mingw32-clang++); machine-specific values can go in local.mk.
-include local.mk
MINGW ?= /Volumes/LEANWORK/tools/llvm-mingw-20261006-ucrt-macos-universal
CXX_W = $(MINGW)/bin/x86_64-w64-mingw32-clang++
WINC = $(MINGW)/generic-w64-mingw32/include
OUT ?= build
CXXFLAGS_W = -O2 -g -std=c++17 -Wall -Wno-unused-function -Iinclude -I$(OUT) -static -static-libgcc -static-libstdc++
STUB_IFACES = ID3D11Device ID3D11DeviceContext ID3D11Buffer ID3D11Texture2D ID3D11ShaderResourceView \
  ID3D11RenderTargetView ID3D11DepthStencilView ID3D11UnorderedAccessView ID3D11VertexShader ID3D11PixelShader \
  ID3D11GeometryShader ID3D11HullShader ID3D11DomainShader ID3D11ComputeShader ID3D11InputLayout ID3D11SamplerState \
  ID3D11BlendState ID3D11RasterizerState ID3D11DepthStencilState IDXGISwapChain

all: $(OUT)/d3d11.dll $(OUT)/d3dr-server.exe $(OUT)/d3dr_triangle.exe $(OUT)/d3d11bench.exe

$(OUT):
	mkdir -p $(OUT)

$(OUT)/stubs.h: tools/gen_stubs.py $(WINC)/d3d11.h $(WINC)/dxgi.h | $(OUT)
	python3 -I tools/gen_stubs.py $@ $(WINC)/d3d11.h $(WINC)/dxgi.h -- $(STUB_IFACES)

$(OUT)/d3d11.dll: src/client.cpp src/d3d11.def include/d3dr_proto.h include/d3dr_format.h $(OUT)/stubs.h
	$(CXX_W) $(CXXFLAGS_W) -shared -o $@ src/client.cpp src/d3d11.def -lws2_32 -luuid

$(OUT)/d3dr-server.exe: src/server.cpp include/d3dr_proto.h | $(OUT)
	$(CXX_W) $(CXXFLAGS_W) -o $@ src/server.cpp -ld3d11 -lws2_32 -luuid

$(OUT)/d3dr_triangle.exe: tests/d3dr_triangle.cpp | $(OUT)
	$(CXX_W) $(CXXFLAGS_W) -o $@ $< -ld3d11 -ld3dcompiler

$(OUT)/d3d11bench.exe: ../wmtr/tests/d3d11bench.cpp | $(OUT)
	$(CXX_W) $(CXXFLAGS_W) -o $@ $< -ld3d11 -ld3dcompiler

$(OUT)/proto_test: tests/proto_test.cpp include/d3dr_proto.h include/d3dr_format.h | $(OUT)
	clang++ -std=c++17 -O1 -g -Wall -Iinclude -o $@ $<

test: $(OUT)/proto_test
	$(OUT)/proto_test

clean:
	rm -rf $(OUT)

.PHONY: all test clean
