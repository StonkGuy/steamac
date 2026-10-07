#!/bin/bash
# DEV-ONLY test helper for the initramfs first-boot provisioning
# (guest/initramfs/init, steamac.provision=1; contract "Docker-free disk
# provisioning", section Payload v1). Stands in for the launcher's Swift disk
# writer: everything here runs in the builder container (Docker), the product
# path does not use this script.
#
#   scripts/test/provision-test-disk.sh disk       work/scratch/provision/test.img: GPT exactly as
#                                                  40-disk.sh + rootfs-A/B = rootfs.img bytes, all
#                                                  other partitions zero; provision.img = payload v1
#   scripts/test/provision-test-disk.sh reference  work/scratch/provision/out/steamos.img: a disk
#                                                  built by the real 40-disk.sh (Docker path)
#   scripts/test/provision-test-disk.sh boot [provision|provision-only]
#                                                  boot test.img headless with NO network and no
#                                                  ssh forward (Steam can never log in) until Steam
#                                                  is ready, then dump a frame (boot-N.png). With
#                                                  "provision": + provision.img as vdc and
#                                                  steamac.provision=1; "provision-only": the same,
#                                                  but power off right after provisioning
#                                                  (steamac.slot=stop), leaving the disk as provisioned.
#                                                  Log: work/scratch/provision/boot-N.log
#   scripts/test/provision-test-disk.sh config PASSWORD
#                                                  work/scratch/provision/config.img = Config payload
#                                                  v1 (new steamos password) for test.img
#   scripts/test/provision-test-disk.sh compare    side-by-side structural comparison
#                                                  test.img vs the reference disk
#   scripts/test/provision-test-disk.sh clean      delete work/scratch/provision
#
# Typical run: disk, reference, boot provision-only, compare, boot provision
# (= idempotent re-run: "already done"), boot (normal), clean.
# Env: HOME_SIZE_GIB (default 8 here), BOOT_SECONDS (default 240),
#      AFTER_READY_SECONDS (default 30), STEAMOS_PASSWORD (default "steamos"),
#      NO_PASSWORD=1 (disk: release-style payload, PASSWORD_HASH=''),
#      PAYLOAD (boot: payload disk instead of provision.img, e.g. a stale one),
#      CONFIG=1 (boot: + config.img and steamac.config=1), CMDLINE_EXTRA (boot:
#      e.g. "steamac.ssh=0"), CONSOLE_SCRIPT (boot: file of "wait ERE" / "send
#      TEXT" / "sleep N" lines run on hvc0 once Steam is ready).
#      TEST_WORK (default work/scratch/provision): private absolute scratch path.
# work/out/steamos.img (the user's disk) is never read or written.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$REPO/work
S=${TEST_WORK:-$WORK/scratch/provision}
export HOME_SIZE_GIB=${HOME_SIZE_GIB:-8}
. "$REPO/scripts/config.env"

in_builder() { # in_builder <cmd...>: builder container, /work = scratch, rootfs cache read-only
    mkdir -p "$S/out"
    local extra=()
    # compare: any other provisioned disk (e.g. one made by the launcher) in place of test.img
    [[ -n ${TEST_IMG:-} ]] && extra=(-v "$(cd "$(dirname "$TEST_IMG")" && pwd)/$(basename "$TEST_IMG"):/work/test.img:ro")
    docker run --rm --privileged --platform linux/arm64 \
        -e HOME_SIZE_GIB -e STEAMOS_PASSWORD -e NO_PASSWORD -e FORCE_DISK=1 \
        -v "$REPO:/src:ro" -v "$S:/work" -v "$WORK/cache/rootfs:/work/cache/rootfs:ro" ${extra[@]+"${extra[@]}"} \
        "$BUILDER_IMAGE" "$@"
}
SELF=/src/scripts/test/provision-test-disk.sh

# ------------------------------------------------------------------ container side
c_disk() {
    . /src/scripts/steps/lib.sh
    local rootfs=/work/cache/rootfs/$STEAMOS_BUILDID/rootfs.img img=/work/test.img
    [[ $(cat "$rootfs.verified" 2>/dev/null) == "$STEAMOS_ROOTFS_SHA256" ]] || { echo "no verified rootfs" >&2; exit 1; }
    local mib=$((1024 * 1024)) home_mib=$((HOME_SIZE_GIB * 1024))
    local total_mib=$((1 + PART_SIZE_ESP + 2 * PART_SIZE_EFI + 2 * PART_SIZE_ROOT + 2 * PART_SIZE_VAR + home_mib + 1))
    local names=(esp efi-A efi-B rootfs-A rootfs-B var-A var-B home)
    local types=(C12A7328-F81F-11D2-BA4B-00A0C93EC93B EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
        EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709
        4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709 4D21B016-B534-45C2-A9FB-5C16E091FD2D
        4D21B016-B534-45C2-A9FB-5C16E091FD2D 933AC7E1-2EB4-4F13-B844-0E14E2AEF915)
    local sizes=(+${PART_SIZE_ESP}M +${PART_SIZE_EFI}M +${PART_SIZE_EFI}M +${PART_SIZE_ROOT}M
        +${PART_SIZE_ROOT}M +${PART_SIZE_VAR}M +${PART_SIZE_VAR}M +${home_mib}M)
    local -A uuid
    local args=(--clear --disk-guid=R) i n
    rm -f "$img" /work/provision.img
    truncate -s $((total_mib * mib)) "$img"
    for i in "${!names[@]}"; do
        n=$((i + 1))
        uuid[${names[i]}]=$(cat /proc/sys/kernel/random/uuid)
        args+=(-n "$n:0:${sizes[i]}" -t "$n:${types[i]}" -c "$n:${names[i]}" -u "$n:${uuid[${names[i]}]}")
    done
    sgdisk -a 2048 "${args[@]}" "$img" >/dev/null
    for n in 4 5; do # rootfs-A, rootfs-B: byte copies, nothing else written
        local start
        start=$(sgdisk -i "$n" "$img" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
        echo "[test-disk] rootfs.img -> partition $n (sector $start)"
        dd if="$rootfs" of="$img" bs=1M seek=$((start * 512 / mib)) conv=sparse,notrunc,fsync status=none
    done
    local guid
    guid=$(sgdisk -p "$img" | sed -n 's/^Disk identifier (GUID): //p' | tr 'A-F' 'a-f')
    local P
    P=$(mktemp -d)
    {
        echo "PROVISION_VERSION='1'"
        echo "STEAMOS_BUILDID='$STEAMOS_BUILDID'"
        echo "STEAMOS_VERSION='$STEAMOS_VERSION'"
        echo "STEAMOS_BRANCH='stable'"
        echo "ROOTFS_SHA256='$STEAMOS_ROOTFS_SHA256'"
        echo "HOSTNAME='steamos'"
        if [[ ${NO_PASSWORD:-} == 1 ]]; then echo "PASSWORD_HASH=''"   # release disks: stock shadow
        else echo "PASSWORD_HASH='$(openssl passwd -6 "${STEAMOS_PASSWORD:-steamos}")'"; fi
        echo "MACHINE_ID='$(tr -d '-' < /proc/sys/kernel/random/uuid)'"
        echo "DISK_GUID='$guid'"
        for n in "${names[@]}"; do
            local k=${n^^}; k=${k//-/_}
            echo "PARTUUID_$k='${uuid[$n],,}'"
        done
    } > "$P/provision.env"
    cp "/work/cache/rootfs/$STEAMOS_BUILDID/rootfs.img.caibx" "$P/rootfs.caibx"
    chmod 0644 "$P/provision.env" "$P/rootfs.caibx"
    (cd "$P" && printf 'provision.env\nrootfs.caibx\n' | cpio --quiet -o -H newc -R 0:0 --reproducible) > /work/provision.img
    truncate -s $(( ($(stat -c %s /work/provision.img) + mib - 1) / mib * mib )) /work/provision.img
    rm -rf "$P"
    echo "[test-disk] /work/test.img ($total_mib MiB), /work/provision.img ($(stat -c %s /work/provision.img) bytes), disk GUID $guid"
}

c_config() { # c_config <password>: /work/config.img = Config payload v1 for /work/test.img
    local guid P mib=$((1024 * 1024))
    guid=$(sgdisk -p /work/test.img | sed -n 's/^Disk identifier (GUID): //p' | tr 'A-F' 'a-f')
    P=$(mktemp -d)
    {
        echo "CONFIG_VERSION='1'"
        echo "DISK_GUID='$guid'"
        echo "PASSWORD_HASH='$(openssl passwd -6 "$1")'"
    } > "$P/config.env"
    chmod 0644 "$P/config.env"
    (cd "$P" && echo config.env | cpio --quiet -o -H newc -R 0:0 --reproducible) > /work/config.img
    truncate -s $(( ($(stat -c %s /work/config.img) + mib - 1) / mib * mib )) /work/config.img
    rm -rf "$P"
    echo "[test-disk] /work/config.img: new steamos password for disk $guid"
}

# report <img> <out>: normalised structural description of one disk
c_report() {
    . /src/scripts/steps/lib.sh
    local img=$1 out=$2 num start end name
    local -A dev u2n
    local loops=() mnts=()
    exec 3>"$out"
    echo "== partition table (sgdisk, GUIDs omitted)" >&3
    sgdisk -p "$img" | sed -n '/^Sector size/,$p' | grep -v 'Disk identifier' >&3
    while read -r num start end; do
        name=$(sgdisk -i "$num" "$img" | sed -n "s/^Partition name: '\(.*\)'/\1/p")
        printf '%s %s type=%s\n' "$num" "$name" "$(sgdisk -i "$num" "$img" | sed -n 's/^Partition GUID code: \([^ ]*\).*/\1/p')" >&3
        u2n[$(sgdisk -i "$num" "$img" | sed -n 's/^Partition unique GUID: //p' | tr 'A-F' 'a-f')]=$name
        dev[$name]=$(newloop -r --offset $((start * 512)) --sizelimit $(((end - start + 1) * 512)) "$img")
        loops+=("${dev[$name]}")
    done < <(sgdisk -p "$img" | awk '/^ +[0-9]+ /{print $1, $2, $3}')
    norm_uuids() { local l k; while IFS= read -r l; do for k in "${!u2n[@]}"; do l=${l//$k/<${u2n[$k]}>}; done; echo "$l"; done; }

    echo "== filesystems (blkid -p)" >&3
    for name in esp efi-A efi-B rootfs-A rootfs-B var-A var-B home; do
        echo "$name: $(blkid -p -o export "${dev[$name]}" | grep -E '^(TYPE|LABEL|VERSION|SEC_TYPE|BLOCK_SIZE)=' | sort | tr '\n' ' ')" >&3
    done
    echo "== FAT parameters (fsck.fat -nv; serials omitted)" >&3
    for name in esp efi-A efi-B; do
        fsck.fat -n -v "${dev[$name]}" 2>&1 | grep -E 'bytes per (logical sector|cluster)|reserved sector|FATs|sectors total|Volume label|data clusters|Data area starts|FAT size' \
            | sed "s/^/$name: /" >&3
    done
    echo "== ext4 parameters (dumpe2fs -h; uuids/times omitted)" >&3
    for name in var-A var-B home; do
        dumpe2fs -h "${dev[$name]}" 2>/dev/null | grep -E '^(Filesystem volume name|Filesystem features|Filesystem flags|Default mount options|Inode count|Block count|Reserved block count|Block size|Inode size|Inodes per group|Blocks per group|Flex block group size|Journal size|Default directory hash|Character encoding|Checksum type|Errors behavior|Reserved GDT blocks|Orphan file size)' \
            | sed "s/^/$name: /;s/  */ /g" >&3
    done
    echo "== btrfs" >&3
    local fa fb
    fa=$(btrfs inspect-internal dump-super "${dev[rootfs-A]}" | awk '$1=="fsid"{print $2}')
    fb=$(btrfs inspect-internal dump-super "${dev[rootfs-B]}" | awk '$1=="fsid"{print $2}')
    echo "rootfs-A sha256 == manifest: $([[ $(sha256sum < "${dev[rootfs-A]}" | cut -d' ' -f1) == "$STEAMOS_ROOTFS_SHA256" ]] && echo yes || echo NO)" >&3
    echo "rootfs-B fsid differs from rootfs-A: $([[ -n $fb && $fa != "$fb" ]] && echo yes || echo NO)" >&3
    echo "rootfs-B superblock flags: $(btrfs inspect-internal dump-super "${dev[rootfs-B]}" | awk '$1=="flags"{print $2, $3}')" >&3
    echo "rootfs-B btrfs check --readonly: $(btrfs check --readonly "${dev[rootfs-B]}" >/dev/null 2>&1 && echo ok || echo FAILED)" >&3
    echo "raw fsid A=$fa B=$fb" > "$out.raw"

    local M
    M=$(mktemp -d)
    for name in esp efi-A efi-B var-A var-B; do
        mkdir -p "$M/$name"; mount -o ro "${dev[$name]}" "$M/$name"; mnts+=("$M/$name")
    done
    for name in esp efi-A efi-B; do
        echo "== $name files" >&3
        (cd "$M/$name" && find . -mindepth 1 | LC_ALL=C sort) >&3
    done
    for f in "$M"/esp/SteamOS/conf/*.conf "$M/esp/steamac/bootenv"; do
        echo "== esp:${f#$M/esp} (sha256 $(sha256sum < "$f" | cut -c1-16))" >&3
        cat "$f" >&3
    done
    for name in efi-A efi-B; do
        for f in "$M/$name"/SteamOS/partsets/*; do
            echo "== $name:${f#$M/$name} (uuids -> <partition name>)" >&3
            norm_uuids < "$f" >&3
        done
    done
    for name in var-A var-B; do
        echo "== $name tree (mode owner size path [-> target])" >&3
        (cd "$M/$name" && find . -mindepth 1 -not -path './lost+found/*' -printf '%M %u:%g %s %p -> %l\n' \
            | sed 's/ -> $//' | LC_ALL=C sort -k4) >&3
        echo "$name machine-id: $(wc -c < "$M/$name/lib/overlays/etc/upper/machine-id") bytes, $(grep -cx '[0-9a-f]\{32\}' "$M/$name/lib/overlays/etc/upper/machine-id") valid line" >&3
        echo "$name shadow (hash replaced): $(sed -E 's/^steamos:\$6\$[^:]*:/steamos:<sha512crypt>:/' "$M/$name/lib/overlays/etc/upper/shadow" | sha256sum | cut -c1-16)" >&3
    done
    echo "machine-id var-A == var-B: $(cmp -s "$M/var-A/lib/overlays/etc/upper/machine-id" "$M/var-B/lib/overlays/etc/upper/machine-id" && echo yes || echo NO)" >&3
    echo "var-A rootfs.caibx == bundle index: $(cmp -s "$M/var-A/lib/steamos-atomupd/rootfs.caibx" "/work/cache/rootfs/$STEAMOS_BUILDID/rootfs.img.caibx" && echo yes || echo NO)" >&3
    for ((i = ${#mnts[@]} - 1; i >= 0; i--)); do umount "${mnts[i]}"; done
    for l in "${loops[@]}"; do losetup -d "$l"; done
    exec 3>&-
}

c_compare() {
    c_report /work/test.img /work/report-test.txt
    c_report /work/out/steamos.img /work/report-ref.txt
    echo "### left: test.img (initramfs-provisioned)        right: reference (40-disk.sh, Docker)"
    diff -y -W 200 /work/report-test.txt /work/report-ref.txt || :
    echo "### raw btrfs fsids: test $(cat /work/report-test.txt.raw) | ref $(cat /work/report-ref.txt.raw)"
    if diff -q /work/report-test.txt /work/report-ref.txt >/dev/null; then
        echo "### RESULT: structurally identical"
    else
        echo "### RESULT: differences (marked | < >):"
        diff /work/report-test.txt /work/report-ref.txt || :
    fi
}

# ------------------------------------------------------------------ host side
# console_script <log> <script>: drive hvc0 (fd 7 = the VM's stdin FIFO) with
# lines "wait ERE" (in console output produced since the last send, 120 s max),
# "send TEXT" (+ newline) and "sleep N".
console_script() {
    local log=$1 line cmd arg off t
    off=$(wc -c < "$log")
    while IFS= read -r line; do
        cmd=${line%% *}; arg=${line#* }
        case $cmd in
            wait)
                t=0
                until tail -c +$((off + 1)) "$log" | tr -d '\r' | grep -qE "$arg"; do
                    sleep 1; t=$((t + 1))
                    ((t < 120)) || { echo "[console] TIMEOUT waiting for: $arg"; return 1; }
                done ;;
            send)  off=$(wc -c < "$log"); printf '%s\n' "$arg" >&7; sleep 1 ;;
            sleep) sleep "$arg" ;;
        esac
    done < "$2"
}

boot() { # boot [provision|provision-only]
    local mode=${1:-} n=1 log extra=
    [[ -f $S/test.img ]] || { echo "run '$0 disk' first" >&2; exit 1; }
    while [[ -e $S/boot-$n.log ]]; do n=$((n + 1)); done
    log=$S/boot-$n.log
    # private copies: the kernel/initramfs/layer in work/out may be rebuilt meanwhile
    cp "$WORK/out/Image" "$WORK/out/initramfs.cpio.gz" "$WORK/out/steamac-layer.img" "$S/"
    case $mode in
        provision) extra=" steamac.provision=1" ;;
        # Explicit initramfs poweroff after provisioning/config, before slot
        # assembly; the disk is left exactly as provisioned for `compare`.
        provision-only) extra=" steamac.provision=1 steamac.slot=stop" ;;
        "") ;;
        *) echo "boot: unknown mode $mode" >&2; exit 2 ;;
    esac
    [[ ${CONFIG:-} == 1 ]] && extra="$extra steamac.config=1"
    extra="$extra${CMDLINE_EXTRA:+ $CMDLINE_EXTRA}"
    local args=(--kernel "$S/Image" --initrd "$S/initramfs.cpio.gz"
        --cmdline "console=hvc0 loglevel=4 rootwait$extra"
        --disk "$S/test.img" --disk "$S/steamac-layer.img:ro")
    [[ -n $mode ]] && args+=(--disk "${PAYLOAD:-$S/provision.img}:ro")
    [[ ${CONFIG:-} == 1 ]] && args+=(--disk "$S/config.img:ro")
    args+=(--no-net --ssh-port 0 --no-sound --headless --cpus 4 --mem 8192
        --frame-dump "$S/boot-$n.png" --log "$log.console")
    echo "[boot-$n] ${args[*]}"
    rm -f "$S/console.in"; mkfifo "$S/console.in"
    exec 7<>"$S/console.in"   # read-write: the VM's stdin never sees EOF
    local pid vm t=0
    if [[ -n ${CONSOLE_SCRIPT:-} ]]; then
        # the launcher forwards stdin to hvc0 only from a terminal: give it a pty
        # (macOS script(1) refuses a non-tty stdin; python's pty.spawn does not)
        /usr/bin/python3 -c 'import pty, sys; sys.exit(pty.spawn(sys.argv[1:]) >> 8)' \
            "$WORK/out/steamac-vm" "${args[@]}" > "$log" 2>&1 < "$S/console.in" &
        pid=$!
        sleep 2; vm=$(pgrep -P $pid | head -1)
    else
        "$WORK/out/steamac-vm" "${args[@]}" > "$log" 2>&1 < "$S/console.in" &
        pid=$!; vm=$pid
    fi
    while kill -0 $pid 2>/dev/null && ((t < ${BOOT_SECONDS:-240})); do
        if [[ $mode != provision-only ]]; then
            grep -q 'progress: ready' "$log" 2>/dev/null && break
        fi
        sleep 2; t=$((t + 2))
    done
    if [[ $mode == provision-only ]]; then
        if kill -0 $pid 2>/dev/null; then
            echo "[boot-$n] ERROR: provisioning VM did not power off after ${t}s ($log)" >&2
            kill -KILL $vm $pid 2>/dev/null || :
            wait $pid 2>/dev/null || :
            exec 7>&-
            rm -f "$S/console.in"
            return 1
        fi
    else
        [[ -n ${CONSOLE_SCRIPT:-} ]] && { console_script "$log" "$CONSOLE_SCRIPT" || :; }
        sleep "${AFTER_READY_SECONDS:-30}"   # let the UI settle before the frame dump
        kill -USR1 $vm 2>/dev/null && sleep 2 || :
        kill -TERM $vm 2>/dev/null || :
        for _ in $(seq 60); do kill -0 $vm 2>/dev/null || break; sleep 1; done
        kill -KILL $vm $pid 2>/dev/null || :
    fi
    local rc=0
    wait $pid 2>/dev/null || rc=$?
    exec 7>&-
    rm -f "$S/console.in"
    echo "[boot-$n] after ${t}s; log $log, console $log.console, frame $S/boot-$n.png"
    grep -E 'steamac-(provision|config)|steamac-init: (provisioning|config|slot|switching|steamac.ssh|FATAL|WARNING)|progress: (provision|ready)|RESULT' "$log" | cut -c1-200 || :
    if [[ $mode == provision-only ]]; then
        ((rc == 0)) && grep -q 'steamac-provision: done' "$log" \
            && ! grep -q 'steamac-provision: failed' "$log" \
            || { echo "[boot-$n] ERROR: provisioning failed (VM exit $rc, $log)" >&2; return 1; }
    fi
}

case ${1:-} in
    _in-container) shift; cmd=$1; shift; "c_$cmd" "$@" ;;
    disk)      in_builder "$SELF" _in-container disk ;;
    reference) in_builder /src/scripts/steps/40-disk.sh ;;
    compare)   in_builder "$SELF" _in-container compare ;;
    boot)      boot "${2:-}" ;;
    config)    in_builder "$SELF" _in-container config "${2:?config PASSWORD}" ;;
    clean)     rm -rf "$S" ;;
    *) sed -n '2,38p' "$0"; exit 2 ;;
esac
