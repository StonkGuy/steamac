#!/bin/sh
# SPDX-License-Identifier: MIT
# guest/mesa/container/verify.sh, runtime step: the ldd -r verdict is PASS only when ldd itself ran and printed a usable
# report. A stub ldd that cannot execute (exit 126), exits 0 with no output, or reports a missing library or an
# undefined symbol must FAIL. The functions are extracted from verify.sh and run with a stub in place of chroot.
# usage: scripts/test/verify-ldd.sh
HERE=$(cd "$(dirname "$0")" && pwd); V="$HERE/../../guest/mesa/container/verify.sh"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; fails=0
awk '/^(check_ldd|check_runtime)\(\) \{/{p=1} p{print} p && /^}/{p=0}' "$V" > "$T/funcs.sh"
[ -s "$T/funcs.sh" ] || { echo "FAIL verify-ldd: check_runtime not found in $V"; exit 1; }

cat > "$T/drv.sh" <<'DRV'
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; }
in_chroot() { if [ "$2" = /usr/bin/ldd ]; then shift 2; sh "$LDD_STUB" "$@"; else echo "OK dlopen"; return 0; fi; }
cp() { :; }
. "$FUNCS"
check_runtime /merged /usr/lib/libx.so vkprobe-x86_64
DRV

case_() { # name want stub-body
    printf '%s\n' "$3" > "$T/ldd.$1"
    out=$(FUNCS="$T/funcs.sh" LDD_STUB="$T/ldd.$1" sh "$T/drv.sh" 2>&1)
    if echo "$out" | grep -q "^$2 ldd -r"; then echo "ok   $1: $2"
    else echo "FAIL $1: want $2"; echo "$out" | sed 's/^/     /'; fails=$((fails + 1)); fi
}
case_ exec126 FAIL 'echo "ldd: /usr/bin/ldd: Exec format error" >&2; exit 126'
case_ silent FAIL 'exit 0'
case_ missing FAIL 'printf "\tlibfoo.so.1 => not found\n"; exit 0'
case_ undefined FAIL 'printf "\tlibc.so.6 => /lib/libc.so.6 (0x1)\nsymbol x: undefined symbol: foo\n"; exit 0'
case_ clean PASS 'printf "\tlinux-vdso.so.1 (0x1)\n\tlibc.so.6 => /lib/libc.so.6 (0x2)\n"; exit 0'

[ $fails -eq 0 ] && echo "verify-ldd: all PASS" || { echo "verify-ldd: $fails FAIL"; exit 1; }
