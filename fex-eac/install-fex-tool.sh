#!/bin/sh
# Swap the patched FEX (Steam-mode build) into Valve's FEX compat tool (Steam app 3127680). Idempotent.
#   install-fex-tool.sh          install   (backs up the Valve files once, keyed by Valve's own VERSIONS.txt)
#   install-fex-tool.sh restore  put the Valve files back
#   install-fex-tool.sh status   print which build is installed
# STAGE=  stage dir from build-fex.sh    BACKUP=  where Valve's files are kept
set -eu
V=$HOME/.local/share/Steam/steamapps/common/FEX-Emu
N=${STAGE:-${WORK:-$HOME/fex-eac-work}/stage}
B=${BACKUP:-$HOME/fex-eac-work/valve-backup}
BINS="FEX FEXBash FEXGetConfig FEXOfflineCompiler FEXServer FEXServerManager FEXpidof"
# Files copied verbatim in both directions: the binaries, the compat tool wrapper, the version stamp and
# the config template (FEX-2610's template enables DiskCache, which the patched build expects).
FILES="FEXCompatTool VERSIONS.txt ConfigTemplate.json"
cur() { sed -n "s/^FEX describe\t//p" "$V/VERSIONS.txt"; }
new() { sed -n "s/^FEX describe\t//p" "$N/VERSIONS.txt"; }
case "${1:-install}" in
status) echo "installed: $(cur)"; echo "staged:    $(new)"; for b in $BINS; do sha256sum "$V/usr/bin/$b" | cut -c1-12; done | tr "\n" " "; echo; exit 0;;
restore) S=$(ls -d "$B"/FEX-* | head -1); for b in $BINS; do cp "$S/usr/bin/$b" "$V/usr/bin/.$b.new" && mv -f "$V/usr/bin/.$b.new" "$V/usr/bin/$b"; done
         for f in $FILES; do cp "$S/$f" "$V/.$f.new" && mv -f "$V/.$f.new" "$V/$f"; done; echo "restored: $(cur)"; exit 0;;
esac
# Already installed when the binaries match the staged build (the describe string is fixed at configure time, so a
# rebuild with a new patch can carry the old one).
same=1; for b in $BINS; do cmp -s "$N/usr/bin/$b" "$V/usr/bin/$b" || same=0; done
[ $same = 1 ] && { echo "already installed: $(cur)"; exit 0; }
# Every file the swap will copy must exist in the stage first: otherwise the copy loop leaves the tool half-old,
# half-new (a mixed FEX, exactly what the FEXServer guard warns about) while still reporting success.
for b in $BINS; do [ -s "$N/usr/bin/$b" ] || { echo "stage incomplete: $N/usr/bin/$b missing or empty" >&2; exit 1; }; done
for f in $FILES; do [ -s "$N/$f" ] || { echo "stage incomplete: $N/$f missing or empty" >&2; exit 1; }; done
# Back up only once, while Valve's build is still in place: a later upgrade of the patched build must not
# overwrite (or add a second) backup with a patched build.
S=$(ls -d "$B"/FEX-* 2>/dev/null | head -1)
if [ -z "$S" ]; then
  S="$B/$(cur)"; mkdir -p "$S/usr/bin"
  for b in $BINS; do cp "$V/usr/bin/$b" "$S/usr/bin/$b"; done; for f in $FILES; do cp "$V/$f" "$S/$f"; done
fi
# FEXServer holds the old binaries open; swapping under it leaves a half-old, half-new tool.
if pgrep -x FEXServer >/dev/null; then echo "FEXServer running: stop Steam games first (or wait for it to exit)"; exit 1; fi
for b in $BINS; do cp "$N/usr/bin/$b" "$V/usr/bin/.$b.new" && mv -f "$V/usr/bin/.$b.new" "$V/usr/bin/$b"; done
for f in $FILES; do cp "$N/$f" "$V/.$f.new" && mv -f "$V/.$f.new" "$V/$f"; done
# Verify the swap actually took, so a partial copy can never be reported as a successful install.
for b in $BINS; do cmp -s "$N/usr/bin/$b" "$V/usr/bin/$b" || { echo "install incomplete: $V/usr/bin/$b does not match the stage" >&2; exit 1; }; done
for f in $FILES; do cmp -s "$N/$f" "$V/$f" || { echo "install incomplete: $V/$f does not match the stage" >&2; exit 1; }; done
echo "installed: $(cur)  (Valve build backed up in $S)"
