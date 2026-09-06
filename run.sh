#!/usr/bin/env bash
# Launch Umbra.
#   ./run.sh              auto-detect the game PID
#   ./run.sh 12345        use a specific PID
#   ./run.sh -q           quiet: a few status lines only
#   ESP_DEBUG=1 ./run.sh  verbose projection/draw logging
#
# Start it whenever you like, including at the menu: it re-resolves its objects
# when a match starts or ends, so it does not need restarting between rounds.
set -u

# Resolve the project directory before changing into build/, so the banner and
# the kernel module are found wherever the project has been moved to.
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ -t 1 ]; then
    RED=$'\033[0;31m'; YEL=$'\033[0;33m'; GRN=$'\033[0;32m'
    DIM=$'\033[2m';    OFF=$'\033[0m'
else
    RED=''; YEL=''; GRN=''; DIM=''; OFF=''
fi

[ -f "$HERE/banner.txt" ] && printf '%s%s%s\n' "$RED" "$(cat "$HERE/banner.txt")" "$OFF"

# ---- kernel module state -----------------------------------------------------
# Read /proc/modules rather than piping lsmod into grep: a matching `grep -q`
# exits early and lsmod dies of SIGPIPE, which misreports under pipefail.
KO="$HERE/kmod/suite_kmod.ko"
if grep -q '^suite_kmod ' /proc/modules 2>/dev/null; then
    printf '%s[kmod] loaded%s  kernel reads and pointer-level input active\n' \
           "$GRN" "$OFF"
elif [ -f "$KO" ]; then
    printf '%s[kmod] built but NOT loaded%s\n' "$YEL" "$OFF"
    printf '%s       sudo insmod %s%s\n' "$DIM" "$KO" "$OFF"
    printf '%s       without it: reads use process_vm_readv, and the mouse is a\n' "$DIM"
    printf '       uinput device visible in /proc/bus/input/devices%s\n' "$OFF"
else
    printf '%s[kmod] not built and not loaded%s\n' "$YEL" "$OFF"
    printf '%s       cd kmod && make && sudo insmod suite_kmod.ko%s\n' "$DIM" "$OFF"
    printf '%s       without it: reads use process_vm_readv, and the mouse is a\n' "$DIM"
    printf '       uinput device visible in /proc/bus/input/devices%s\n' "$OFF"
fi
echo

cd "$HERE/build" || { echo "no build/ directory; run ./build.sh first"; exit 1; }

QUIET=0
PID=""
for a in "$@"; do
    case "$a" in
        -q|--quiet) QUIET=1 ;;
        *)          PID="$a" ;;
    esac
done

if [ -z "$PID" ]; then
    PID=$(grep -rl 'Discovery-d.exe' /proc/*/maps 2>/dev/null \
          | head -1 | cut -d/ -f3)
    [ -n "$PID" ] && [ "$(cat /proc/$PID/comm 2>/dev/null)" = "GameThread" ] || {
        for p in $(grep -rl 'Discovery-d.exe' /proc/*/maps 2>/dev/null | cut -d/ -f3); do
            [ "$(cat /proc/$p/comm 2>/dev/null)" = "GameThread" ] && PID=$p && break
        done
    }
fi
[ -z "${PID:-}" ] && { echo "Game not found. Is it running?"; exit 1; }

if [ "$QUIET" = 1 ]; then
    # Collapse the ESP's log to a few milestones. stdbuf forces line buffering
    # so lines through the pipe appear promptly; awk prints each milestone once.
    echo "[run] started (pid $PID)"
    stdbuf -oL -eL ./TheFinals --pid "$PID" 2>&1 | stdbuf -oL awk '
        /kernel backend: \/dev\/suite_kmod/ && !km {
            print "[run] kernel memory: functional"; km=1; fflush(); next }
        /kernel injection into the real pointer/ && !ms {
            print "[run] kernel mouse: functional";  ms=1; fflush(); next }
        /PlayerArray\.Num/ && !wk {
            print "[run] working - reading the match"; wk=1; fflush(); next }
        { next }'
    exit 0
fi

echo "[run] PID $PID ($(cat /proc/$PID/comm 2>/dev/null))"
echo "[run] starting overlay. INSERT = menu, End = quit."
echo "[run] you can start at the menu; it picks up matches on its own."
./TheFinals --pid "$PID"
