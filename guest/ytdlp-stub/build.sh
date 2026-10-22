#!/bin/bash
# SPDX-License-Identifier: MIT
# Build yt-dlp.exe (the steamac yt-dlp stub, x86-64 Windows, no C runtime) into ${1:-work/out/ytdlp-stub}.
# Uses x86_64-w64-mingw32-gcc when present, else LLVM (clang + lld-link + llvm-dlltool, e.g. Homebrew llvm).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/../../work/out/ytdlp-stub}
mkdir -p "$out"
if command -v x86_64-w64-mingw32-gcc >/dev/null; then
    x86_64-w64-mingw32-gcc -Os -Wall -Werror -nostdlib -ffreestanding -fno-stack-protector -mno-stack-arg-probe \
        -e mainCRTStartup -s -o "$out/yt-dlp.exe" "$here/yt-dlp-stub.c" -lws2_32 -lkernel32
else
    llvm=${LLVM_BIN:-$(brew --prefix llvm 2>/dev/null)/bin}
    t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
    # import libraries for the few functions used (no Windows SDK needed)
    printf 'LIBRARY kernel32.dll\nEXPORTS\n%s\n' GetStdHandle WriteFile GetCommandLineW GetModuleFileNameW \
        CreateProcessW WaitForSingleObject GetExitCodeProcess CloseHandle WideCharToMultiByte ExitProcess > "$t/kernel32.def"
    printf 'LIBRARY ws2_32.dll\nEXPORTS\n%s\n' WSAStartup socket connect send recv closesocket htons htonl > "$t/ws2_32.def"
    for l in kernel32 ws2_32; do "$llvm/llvm-dlltool" -m i386:x86-64 -d "$t/$l.def" -l "$t/$l.lib"; done
    "$llvm/clang" --target=x86_64-w64-windows-gnu -Os -Wall -Werror -ffreestanding -fno-stack-protector \
        -mno-stack-arg-probe -c -o "$t/stub.o" "$here/yt-dlp-stub.c"
    "$( [[ -x $llvm/lld-link ]] && echo "$llvm/lld-link" || command -v lld-link)" /nologo /subsystem:console /entry:mainCRTStartup /nodefaultlib /opt:ref \
        /out:"$out/yt-dlp.exe" "$t/stub.o" "$t/kernel32.lib" "$t/ws2_32.lib"
fi
echo "built $out/yt-dlp.exe ($(wc -c < "$out/yt-dlp.exe") bytes)"
