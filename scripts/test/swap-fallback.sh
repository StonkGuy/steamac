#!/bin/bash
# SPDX-License-Identifier: MIT
# guest/layer/usr/lib/steamac/swap-fallback (steamac.swap_fallback=) and the zram drop-in, with swapon, swapoff,
# mkswap, fallocate and df stubbed:
#   default  no swap file yet, enough space: half of MemTotal allocated, mkswap'd, enabled at priority 10
#   again    file in use: nothing is redone
#   reboot   file exists, not in use: enabled without a new mkswap
#   space    less than 3x the size free on /home: no file, no swapon
#   off      steamac.swap_fallback=0: an active file is turned off and removed
#   units    the service is enabled and runs the script once; zram0 = RAM size with zstd, above the file's priority
# usage: scripts/test/swap-fallback.sh
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)/guest/layer
S=$R/usr/lib/steamac/swap-fallback
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fails=0; fail() { fails=$((fails + 1)); echo "FAIL [$1]: $2"; }
mkdir "$T/bin" "$T/home"
printf 'MemTotal: 8108884 kB\nMemFree: 1000 kB\n' > "$T/meminfo"
cat > "$T/bin/swapon" <<'X'
#!/bin/bash
if [ "$1" = --show=NAME ]; then cat "$STUB/active" 2>/dev/null; exit 0; fi
echo "swapon $*" >> "$STUB/log"; echo "${@: -1}" >> "$STUB/active"
X
cat > "$T/bin/swapoff" <<'X'
#!/bin/bash
echo "swapoff $*" >> "$STUB/log"; grep -vxF "$1" "$STUB/active" > "$STUB/active.n"; mv "$STUB/active.n" "$STUB/active"
X
printf '#!/bin/bash\necho "mkswap $*" >> "$STUB/log"\n' > "$T/bin/mkswap"
printf '#!/bin/bash\necho "fallocate $*" >> "$STUB/log"; : > "$3"\n' > "$T/bin/fallocate"
printf '#!/bin/bash\necho "     Avail"; cat "$STUB/avail"\n' > "$T/bin/df"
chmod +x "$T/bin/"*
F=$T/home/steamac-swap

run() { # cmdline
    printf '%s\n' "$1" > "$T/cmdline"; : > "$T/log"
    STUB=$T PATH=$T/bin:$PATH STEAMAC_CMDLINE=$T/cmdline STEAMAC_MEMINFO=$T/meminfo STEAMAC_SWAPFILE=$F \
        bash "$S" > "$T/out" 2>&1
}

echo 20000 > "$T/avail"
run "console=hvc0 rootwait" || fail default "exit status $?"
grep -qx "fallocate -l 3959M $F" "$T/log" && grep -q "^mkswap -q $F" "$T/log" && grep -qx "swapon -p 10 $F" "$T/log" \
    || fail default "log: $(tr '\n' ';' < "$T/log")"
[ "$(stat -f %Lp "$F" 2>/dev/null || stat -c %a "$F")" = 600 ] || fail default "swap file mode not 600"

run "rootwait" || fail again "exit status $?"
[ ! -s "$T/log" ] || fail again "redone while in use: $(tr '\n' ';' < "$T/log")"

: > "$T/active"
run "rootwait" || fail reboot "exit status $?"
grep -q fallocate "$T/log" && fail reboot "file allocated again"
grep -q mkswap "$T/log" && fail reboot "mkswap again (would drop nothing, but costs a write)"
grep -qx "swapon -p 10 $F" "$T/log" || fail reboot "not enabled: $(tr '\n' ';' < "$T/log")"

run "steamac.swap_fallback=0" || fail off "exit status $?"
grep -qx "swapoff $F" "$T/log" && [ ! -e "$F" ] || fail off "still on or still there: $(tr '\n' ';' < "$T/log")"
grep -q '^swapon ' "$T/log" && fail off "enabled although steamac.swap_fallback=0"

echo 11000 > "$T/avail"; : > "$T/active"
run "rootwait" || fail space "exit status $?"
[ ! -e "$F" ] && [ ! -s "$T/log" ] || fail space "created with 11000 MiB free: $(tr '\n' ';' < "$T/log")"

U=$R/usr/lib/systemd/system
[ "$(readlink "$U/multi-user.target.wants/steamac-swap-fallback.service")" = ../steamac-swap-fallback.service ] \
    || fail units "service not enabled"
grep -qx 'Type=oneshot' "$U/steamac-swap-fallback.service" && grep -qx 'ExecStart=/usr/lib/steamac/swap-fallback' "$U/steamac-swap-fallback.service" \
    || fail units "service does not run the script once"
grep -qx 'RequiresMountsFor=/home' "$U/steamac-swap-fallback.service" || fail units "service may run before /home is mounted"
Z=$R/usr/lib/systemd/zram-generator.conf.d/50-steamac.conf
grep -qx '\[zram0\]' "$Z" && grep -qx 'zram-size = ram' "$Z" && grep -qx 'compression-algorithm = zstd' "$Z" \
    || fail units "zram drop-in"
grep -q 'swap-priority' "$Z" && fail units "zram drop-in changes the priority (must stay above the file's 10)"
[ -x "$S" ] && bash -n "$S" || fail units "script not executable or invalid"

[ $fails -eq 0 ] && echo "swap-fallback: all PASS" || { echo "swap-fallback: $fails FAIL"; exit 1; }
