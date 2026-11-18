#!/bin/sh
# SPDX-License-Identifier: MIT
# Checks fex-eac/build-fex.sh's source handling without building anything:
#   1. a tree left by an older build-fex.sh (upstream FEX-2610 + patch commits) is refused with the command to remove it;
#   2. with ONLINE=1, the pinned FEX_EAC_SHA is what tag FEX_EAC_TAG points at on StonkGuy/FEX.
# usage: scripts/test/build-fex-pin.sh
HERE=$(cd "$(dirname "$0")" && pwd); S="$HERE/../../fex-eac/build-fex.sh"; fail=0
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
git init -q "$d/FEX" && git -C "$d/FEX" -c user.name=t -c user.email=t@invalid commit -q --allow-empty -m "Docs: Update for release FEX-2610"
git -C "$d/FEX" -c user.name=build -c user.email=build@invalid commit -q --allow-empty -m "LinuxSyscalls: emulate x86-64 ptrace"
out=$(WORK="$d" sh "$S" 2>&1); rc=$?
if [ $rc -ne 0 ] && printf '%s' "$out" | grep -q "rm -rf '$d/FEX'"; then echo "ok   old tree refused"; else echo "FAIL old tree: rc=$rc: $out"; fail=1; fi
[ -d "$d/FEX/.git" ] && echo "ok   old tree left in place" || { echo "FAIL old tree was removed"; fail=1; }
pin=$(sed -n 's/^FEX_EAC_SHA=//p' "$S"); mkdir -p "$d/bin" "$d/w/FEX/.git" "$d/w/stage"
printf '#!/bin/sh\necho %s\n' "$pin" > "$d/bin/git"
printf '#!/bin/sh\ncase " $* " in *" --install "*) mkdir -p "$DESTDIR/usr/bin" && : > "$DESTDIR/usr/bin/FEX"; [ -z "$FAIL" ];; esac\n' > "$d/bin/cmake"
chmod +x "$d/bin/git" "$d/bin/cmake"; echo "$pin" > "$d/w/stage/.steamac-fex-source"
FAIL=1 PATH="$d/bin:$PATH" WORK="$d/w" sh "$S" > /dev/null 2>&1; rc=$?
if [ $rc -ne 0 ] && [ ! -e "$d/w/stage/.steamac-fex-source" ]; then echo "ok   failed install drops the old receipt"; else echo "FAIL failed install: rc=$rc, receipt kept"; fail=1; fi
rm -f "$d/w/stage/.steamac-fex-source"; PATH="$d/bin:$PATH" WORK="$d/w" sh "$S" > /dev/null 2>&1; rc=$?
if [ $rc -eq 0 ] && [ "$(cat "$d/w/stage/.steamac-fex-source" 2>/dev/null)" = "$pin" ]; then echo "ok   staged build publishes receipt $pin"; else echo "FAIL staged build: rc=$rc, no receipt $pin"; fail=1; fi
printf '#!/bin/sh\nif [ "$1" = diff ]; then exit 1; fi\necho %s\n' "$pin" > "$d/bin/git"
PATH="$d/bin:$PATH" WORK="$d/w" sh "$S" > "$d/dirty-out" 2>&1; rc=$?
if [ $rc -ne 0 ] && grep -q 'local changes' "$d/dirty-out"; then echo "ok   locally modified source is refused"; else echo "FAIL locally modified source was accepted: rc=$rc"; fail=1; fi
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@invalid GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@invalid
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=protocol.file.allow GIT_CONFIG_VALUE_0=always
tag=$(sed -n 's/^FEX_EAC_TAG=//p' "$S"); mkdir "$d/c" && cp "$d/bin/cmake" "$d/c/"
git init -q "$d/sub" && echo s > "$d/sub/f" && git -C "$d/sub" add f && git -C "$d/sub" commit -qm s
git init -q "$d/up" && git -C "$d/up" submodule -q add "$d/sub" ext && git -C "$d/up" commit -qm old && git -C "$d/up" tag eac-2610.2
git clone -q --recurse-submodules --branch eac-2610.2 "$d/up" "$d/old/FEX" 2>/dev/null
echo t > "$d/sub/f" && git -C "$d/sub" commit -qam t && git -C "$d/up/ext" pull -q && git -C "$d/up" commit -qam new && git -C "$d/up" tag "$tag"; new=$(git -C "$d/up" rev-parse HEAD)
sed -e "s|^REPO=.*|REPO=$d/up|" -e "s|^FEX_EAC_SHA=.*|FEX_EAC_SHA=$new|" "$S" > "$d/build.sh"
PATH="$d/c:$PATH" WORK="$d/old" sh "$d/build.sh" > "$d/old-out" 2>&1; rc=$?
if [ $rc -eq 0 ] && [ "$(git -C "$d/old/FEX" rev-parse HEAD)" = "$new" ] && [ "$(cat "$d/old/FEX/ext/f")" = t ]; then echo "ok   clean clone of an older tag fetches $tag"; else echo "FAIL older clean clone: rc=$rc: $(cat "$d/old-out")"; fail=1; fi
git clone -q --branch "$tag" "$d/up" "$d/part/FEX" 2>/dev/null
PATH="$d/c:$PATH" WORK="$d/part" sh "$d/build.sh" > "$d/part-out" 2>&1; rc=$?
if [ $rc -eq 0 ] && [ -f "$d/part/FEX/ext/f" ]; then echo "ok   clone with uninitialized submodules recovers"; else echo "FAIL partial clone: rc=$rc, submodule missing: $(cat "$d/part-out")"; fail=1; fi
echo x > "$d/part/FEX/ext/f"; PATH="$d/c:$PATH" WORK="$d/part" sh "$d/build.sh" > "$d/part-out" 2>&1; rc=$?
if [ $rc -ne 0 ] && grep -q 'local changes' "$d/part-out" && [ "$(cat "$d/part/FEX/ext/f")" = x ]; then echo "ok   modified submodule is refused and kept"; else echo "FAIL modified submodule: rc=$rc"; fail=1; fi
if [ "${ONLINE:-0}" = 1 ]; then
  tag=$(sed -n 's/^FEX_EAC_TAG=//p' "$S"); sha=$(sed -n 's/^FEX_EAC_SHA=//p' "$S")
  # an annotated tag lists its commit as tag^{}; a lightweight tag only as the tag itself
  refs=$(git ls-remote https://github.com/StonkGuy/FEX.git "refs/tags/$tag" "refs/tags/$tag^{}")
  got=$(printf '%s\n' "$refs" | awk -v t="refs/tags/$tag^{}" '$2 == t {print $1}')
  [ -n "$got" ] || got=$(printf '%s\n' "$refs" | awk -v t="refs/tags/$tag" '$2 == t {print $1}')
  [ "$got" = "$sha" ] && echo "ok   $tag = $sha" || { echo "FAIL $tag is '$got', script pins $sha"; fail=1; }
fi
exit $fail
