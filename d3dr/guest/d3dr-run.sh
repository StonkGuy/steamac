#!/bin/bash
# d3dr-run.sh EXE [ARGS...]: run a Windows test program inside the steamac guest with Proton's Wine under the Steam
# FEX tool and d3dr's d3d11.dll (copied next to the program, loaded native), so its Direct3D 11 calls go to
# d3dr-server.exe on the Mac. Scratch prefix; never touches Steam's compatdata or settings.
# Env: D3DR_SERVER (default 192.168.127.254:47811 = the Mac's 127.0.0.1 through gvproxy), D3DR_DLL (default
# ~/vrc/d3dr/d3d11.dll), PROTON (default Proton - Experimental), FEX, D3DR_DISPLAY (default :1).
set -u
EXE=$1; shift
D=$HOME/vrc/d3dr
P=${PROTON:-"$HOME/.local/share/Steam/steamapps/common/Proton - Experimental"}/files
DLL=${D3DR_DLL:-$D/d3d11.dll}
# Proton keeps vkd3d-shader (needed by wined3d -> d3dcompiler_47) outside the default DLL path
for f in "$P"/lib/vkd3d/x86_64-windows/*.dll; do [ -e "$(dirname "$EXE")/${f##*/}" ] || cp -f "$f" "$(dirname "$EXE")/"; done
cmp -s "$DLL" "$(dirname "$EXE")/d3d11.dll" || cp -f "$DLL" "$(dirname "$EXE")/d3d11.dll" || exit 1
export WINEPREFIX=$D/pfx
export WINEDLLOVERRIDES="mscoree,mshtml=;d3d11=n"
export D3DR_SERVER=${D3DR_SERVER:-192.168.127.254:47811}
FEX=${FEX:-$HOME/.local/share/Steam/steamapps/common/FEX-Emu/usr/bin/FEX}
export FEX_ROOTFS=/usr/share/guestos/fex-mesa FEX_NEEDSSECCOMP=1
export LD_LIBRARY_PATH="$P/lib/x86_64-linux-gnu:$P/lib:$P/lib64"
export DISPLAY=${D3DR_DISPLAY:-:1}
export WINEDEBUG=${WINEDEBUG:--all}
exec "$FEX" "$P/bin/wine" "$EXE" "$@"
