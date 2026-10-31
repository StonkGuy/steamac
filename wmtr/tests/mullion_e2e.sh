#!/bin/bash
# mullion_e2e.sh: tests/d3d12_memory.exe through the wmtr two-process path with Mullion, on a Mac (CI: Mullion workflow,
# job e2e): an x86-64 macOS Wine (Rosetta) with Mullion's PE dlls in place of its Direct3D and the wmtr client
# winemetal.so (socket transport), and wmtr-server running Mullion's unix side on the Mac's GPU. The client's
# marshalling is the guest's (the VM transport differs only below it), so a Mullion call wmtr carries wrongly fails
# here as it does in the VM: before host/mullion patch 0003 the compute cases failed (the root-argument arena was
# memory of Mullion's own, which the executor never saw).
#   WINE=<dir with bin/wine64> PE=<Mullion's PE dlls> CLIENT=<x86_64-macos winemetal.so> SERVER=<wmtr-server>
#   EXE=<d3d12_memory.exe> [OUT=<dir>] [MULLION_CONFIG=...] wmtr/tests/mullion_e2e.sh
# Prints the program's report; exit status 0 only for "RESULT ... failed=0". OUT keeps the logs (server, wineboot,
# run) and the report.
set -u
for v in WINE PE CLIENT SERVER EXE; do [ -e "${!v:-}" ] || { echo "mullion_e2e: $v=${!v:-} missing" >&2; exit 2; }; done
O=${OUT:-$(mktemp -d)}
rm -rf "$O"; mkdir -p "$O"
W=$O/wine
# a copy of the Wine: Mullion's libraries go where it keeps its own (Mullion's install.sh), the client as winemetal.so
cp -R "$WINE" "$W"
cp "$PE"/*.dll "$W/lib/wine/x86_64-windows/"
cp "$CLIENT" "$W/lib/wine/x86_64-unix/winemetal.so"
export WINEPREFIX=$O/pfx WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="mscoree,mshtml,winemenubuilder.exe="
export WMTR_SOCKET=$O/wmtr.sock MULLION_CONFIG=${MULLION_CONFIG-d3d12.cpuVisibleHeapMemory=True}
# a hard time limit (macOS has no timeout(1))
limit() { local s=$1; shift; perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "$s" "$@"; }
"$SERVER" > "$O/server.log" 2>&1 &
SP=$!
trap 'kill $SP 2>/dev/null; "$W/bin/wineserver" -k 2>/dev/null' EXIT
for _ in $(seq 50); do [ -S "$WMTR_SOCKET" ] && break; sleep 0.2; done
[ -S "$WMTR_SOCKET" ] || { echo "mullion_e2e: wmtr-server did not listen on $WMTR_SOCKET"; cat "$O/server.log"; exit 1; }
limit 600 "$W/bin/wine64" wineboot -u > "$O/wineboot.log" 2>&1 || { echo "mullion_e2e: wineboot failed"; tail -20 "$O/wineboot.log"; exit 1; }
# a prefix made before Mullion's libraries were in the Wine does not know the ones Wine lacks (install.sh)
cp "$PE"/*.dll "$WINEPREFIX/drive_c/windows/system32/"
echo "mullion_e2e: $(basename "$EXE"), MULLION_CONFIG=${MULLION_CONFIG:-(empty)}"
limit 300 "$W/bin/wine64" "$EXE" --out "Z:${O//\//\\}\\report.txt" > "$O/run.log" 2>&1
status=$?
echo "mullion_e2e: exit status $status"
if [ ! -s "$O/report.txt" ]; then
  echo "mullion_e2e: no report; run log and server log:"; tail -30 "$O/run.log"; tail -30 "$O/server.log"; exit 1
fi
cat "$O/report.txt"
if ! grep -q '^RESULT passed=[0-9]* failed=0 skipped=' "$O/report.txt"; then
  grep -q '^RESULT' "$O/report.txt" || echo "mullion_e2e: no RESULT line: the program ended in the case after the last CASE line"
  echo "mullion_e2e: server log:"; tail -30 "$O/server.log"
  exit 1
fi
echo "mullion_e2e: passed"
