#!/bin/sh
# The guest's virtio-port udev rules must not make a port world-accessible: a port that a session agent opens is
# owned by that session user (steamos) with mode 0660, never 0666.
cd "$(dirname "$0")/../../guest/layer/usr/lib/udev/rules.d" || exit 1; fail=0
for f in 70-fx-*.rules; do
  if grep -v '^#' "$f" | grep -q 'MODE="0666"'; then echo "FAIL $f: world-accessible port (MODE=0666)"; fail=1; fi
  grep -v '^#' "$f" | grep 'MODE=' | grep -qv 'OWNER=' && { echo "FAIL $f: MODE without OWNER"; fail=1; }
done
[ "$fail" = 0 ] && echo "PASS udev rules: no world-accessible virtio port"
exit $fail
