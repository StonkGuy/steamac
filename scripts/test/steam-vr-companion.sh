#!/bin/bash
# SPDX-License-Identifier: MIT
# guest/layer/usr/lib/steamac/steam-vr-companion (+ steamac-vr-companion, steamac-vdf) against a fake Steam tree:
#   on       steamac.vr=1 steamac.vr_companion=2370570: tools in the state dir (with the user's steam_api64.dll), the
#            app's launch option is the wrapper, its compatibility tool Proton Experimental; other apps untouched
#   again    a second run changes nothing
#   own      an app with the user's own launch options keeps them
#   wrapper  the launch wrapper puts svr-driverhost.exe in place of the .exe and passes the .exe and its arguments
#            after --then; without svr-driverhost.exe the command runs unchanged
#   watch    the watcher starts the app once per VR session (mirror sequence moving), again after a pause
#   off      launch option and compatibility tool back, state dir gone, the rest of both files unchanged
# usage: scripts/test/steam-vr-companion.sh
set -u
L=$(cd "$(dirname "$0")/../.." && pwd)/guest/layer/usr/lib/steamac
S=$L/steam-vr-companion
T=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
fails=0; fail() { fails=$((fails + 1)); echo "FAIL [$1]: $2"; }
ST=$T/steam; mkdir -p "$ST/userdata/1234/config" "$ST/config"
cat > "$ST/userdata/1234/config/localconfig.vdf" <<'EOF'
"UserLocalConfigStore"
{
	"Software"
	{
		"Valve"
		{
			"Steam"
			{
				"apps"
				{
					"438100"
					{
						"LastPlayed"		"1792658228"
					}
					"999"
					{
						"LaunchOptions"		"PROTON_LOG=1 %command%"
					}
				}
			}
		}
	}
	"friends"
	{
		"PersonaName"		"x"
	}
}
EOF
cat > "$ST/config/config.vdf" <<'EOF'
"InstallConfigStore"
{
	"Software"
	{
		"Valve"
		{
			"Steam"
			{
				"CompatToolMapping"
				{
					"438100"
					{
						"name"		"proton_experimental"
						"config"		""
						"priority"		"250"
					}
				}
			}
		}
	}
}
EOF
cp "$ST/userdata/1234/config/localconfig.vdf" "$T/local.orig"; cp "$ST/config/config.vdf" "$T/config.orig"
P=$T/vr; mkdir -p "$P/tools"; echo exe > "$P/tools/svr-driverhost.exe"
DLL=$T/steam_api64.dll; echo dll > "$DLL"
VDF="python3 $L/steamac-vdf"
run() { echo "$1" > "$T/cmdline"
  STEAMAC_CMDLINE=$T/cmdline STEAMAC_STEAM_ROOT=$ST STEAMAC_VR_DIR=$P STEAMAC_VRC_STATE=$T/state STEAMAC_VRC_DLL=$DLL \
    HOME=$T bash "$S" "${@:2}" 2>>"$T/log"; }
LC=$ST/userdata/1234/config/localconfig.vdf CF=$ST/config/config.vdf
KEY=UserLocalConfigStore/Software/Valve/Steam/apps CK=InstallConfigStore/Software/Valve/Steam/CompatToolMapping

# on
run "steamac.vr=1 steamac.vr_companion=2370570,999"
[[ $($VDF get "$LC" "$KEY/2370570/LaunchOptions") == "$T/state/tools/steamac-vr-companion %command%" ]] || fail on "launch option"
[[ $($VDF get "$CF" "$CK/2370570/name") == proton_experimental ]] || fail on "compat tool"
[[ -f $T/state/tools/svr-driverhost.exe && -x $T/state/tools/steamac-vr-companion && -f $T/state/tools/steam_api64.dll ]] || fail on "tools"
[[ $($VDF get "$LC" "$KEY/438100/LastPlayed") == 1792658228 && $($VDF get "$LC" "UserLocalConfigStore/friends/PersonaName") == x ]] || fail on "other keys"
# own
[[ $($VDF get "$LC" "$KEY/999/LaunchOptions") == "PROTON_LOG=1 %command%" ]] || fail own "user's launch options replaced"
grep -q "app 999 has the user's own launch options" "$T/log" || fail own "no log line"
# again
a=$(cksum < "$LC"; cksum < "$CF"); run "steamac.vr=1 steamac.vr_companion=2370570,999"; b=$(cksum < "$LC"; cksum < "$CF")
[[ $a == "$b" ]] || fail again "files changed on a second run"

# wrapper
W=$T/state/tools/steamac-vr-companion
printf '#!/bin/sh\nfor a; do echo "[$a]"; done\n' > "$T/proton"; chmod +x "$T/proton"
out=$("$W" "$T/proton" waitforexitandrun "/games/Standable/Standable.exe" -x 2>/dev/null | tr -d '\n')
[[ $out == "[waitforexitandrun][$T/state/tools/svr-driverhost.exe][--then][/games/Standable/Standable.exe][-x]" ]] || fail wrapper "got $out"
mv "$T/state/tools/svr-driverhost.exe" "$T/x.exe"
out=$("$W" "$T/proton" waitforexitandrun "/games/Standable/Standable.exe" 2>/dev/null | tr -d '\n')
[[ $out == "[waitforexitandrun][/games/Standable/Standable.exe]" ]] || fail wrapper "without the host: $out"
mv "$T/x.exe" "$T/state/tools/svr-driverhost.exe"

# watch: mirror seq at byte 16 of the table
TB=$T/table; head -c 8080 /dev/zero > "$TB"
setseq() { python3 -c 'import struct,sys; f=open(sys.argv[1],"r+b"); f.seek(16); f.write(struct.pack("<I",int(sys.argv[2])))' "$TB" "$1"; }
printf '#!/bin/sh\necho "$@" >> %s/steam-calls\n' "$T" > "$T/fake-steam"; chmod +x "$T/fake-steam"
echo "steamac.vr=1 steamac.vr_companion=2370570" > "$T/cmdline"
STEAMAC_CMDLINE=$T/cmdline STEAMAC_VR_TRACKERS=$TB STEAMAC_STEAM_CMD=$T/fake-steam STEAMAC_VRC_POLL=1 STEAMAC_VRC_FOREGROUND=1 \
  bash "$S" --watch 2>>"$T/log" &
wp=$!
sleep 2.5
[[ ! -f $T/steam-calls ]] || fail watch "started without a VR game"
for i in 1 2 3 4 5; do setseq $((i * 2)); sleep 1; done
[[ $(wc -l < "$T/steam-calls" 2>/dev/null | tr -d " ") == 1 ]] || fail watch "first session: $(cat "$T/steam-calls" 2>/dev/null)"
grep -q -- "-ifrunning steam://rungameid/2370570" "$T/steam-calls" || fail watch "command"
sleep 12   # game gone (no movement for 10 s), then a new session
for i in 6 7 8 9; do setseq $((i * 2)); sleep 1; done
[[ $(wc -l < "$T/steam-calls" | tr -d " ") == 2 ]] || fail watch "second session: $(wc -l < "$T/steam-calls" | tr -d " ") starts"
kill $wp 2>/dev/null

# off
run "steamac.vr=1"
[[ ! -d $T/state ]] || fail off "state dir left"
python3 -B - "$T/local.orig" "$LC" "$T/config.orig" "$CF" "$L/steamac-vdf" <<'EOF' || fail off "files differ from the originals"
import importlib.machinery, importlib.util, sys
loader = importlib.machinery.SourceFileLoader("vdf", sys.argv[5])
spec = importlib.util.spec_from_loader("vdf", loader); m = importlib.util.module_from_spec(spec); loader.exec_module(m)
for a, b in ((sys.argv[1], sys.argv[2]), (sys.argv[3], sys.argv[4])):
    x, y = m.parse(open(a).read()), m.parse(open(b).read())
    if x != y:
        # an app section the wrapper created stays (empty): Steam itself keeps such sections
        def strip(n): return [[k, strip(v)] if isinstance(v, list) else [k, v] for k, v in n if not (isinstance(v, list) and not v)]
        if strip(x) != strip(y): print(a, x, y); sys.exit(1)
EOF
[[ -z $($VDF get "$CF" "$CK/2370570/name" 2>/dev/null) ]] || fail off "compat tool left"

if (( fails )); then cat "$T/log"; echo "FAIL steam-vr-companion: $fails"; exit 1; fi
echo "PASS steam-vr-companion"
