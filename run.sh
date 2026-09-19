#!/usr/bin/env bash
# Launch Umbra.
#   ./run.sh              auto-detect the game PID
#   ./run.sh 12345        use a specific PID
#   ./run.sh -q           quiet: a few status lines only, and no update check
#   ESP_DEBUG=1 ./run.sh  verbose projection/draw logging
#
# Start it whenever you like, including at the menu: it re-resolves its objects
# when a match starts or ends, so it does not need restarting between rounds.
set -u

# Resolve the project directory before changing into build/, so the banner and
# the kernel module are found wherever the project has been moved to.
HERE="$(cd "$(dirname "$0")" && pwd)"

QUIET=0
PID=""
for a in "$@"; do
    case "$a" in
        -q|--quiet) QUIET=1 ;;
        *)          PID="$a" ;;
    esac
done

if [ -t 1 ]; then
    RED=$'\033[0;31m'; YEL=$'\033[0;33m'; GRN=$'\033[0;32m'
    DIM=$'\033[2m';    OFF=$'\033[0m'
else
    RED=''; YEL=''; GRN=''; DIM=''; OFF=''
fi

# ---- update check ------------------------------------------------------------
# Says so when GitHub has a newer release than this copy, and nothing else: it
# downloads nothing and changes nothing. It runs in the background, so a slow or
# missing network never holds up the start. The newest release is read from
# where the latest-release page redirects, which needs no API and no rate limit.
REPO="Ymsniper/umbra"
check_update() {
    local mine url="" tag latest
    mine=$(sed -n 's/.*kUmbraVersion *= *"\([^"]*\)".*/\1/p' "$HERE/src/main.cpp" 2>/dev/null)
    [ -n "$mine" ] || return 0
    if command -v curl >/dev/null 2>&1; then
        url=$(curl -sIL -o /dev/null -w '%{url_effective}' --max-time 5 \
              "https://github.com/$REPO/releases/latest" 2>/dev/null)
    elif command -v wget >/dev/null 2>&1; then
        url=$(wget -S --spider -T 5 -t 1 "https://github.com/$REPO/releases/latest" 2>&1 \
              | sed -n 's/^ *[Ll]ocation: *//p' | tail -n 1)
    fi
    case "$url" in */releases/tag/*) ;; *) return 0 ;; esac
    tag=${url##*/releases/tag/}
    tag=${tag%%[[:space:]/?#]*}
    latest=${tag#v}
    [[ $latest =~ ^[0-9]+(\.[0-9]+)*$ ]] || return 0
    [ "$latest" != "$mine" ] || return 0
    [ "$(printf '%s\n%s\n' "$mine" "$latest" | sort -V | tail -n 1)" = "$latest" ] || return 0
    # Past this point the script may already have ended, when the game was not
    # found; a line arriving after the prompt has come back would only confuse.
    kill -0 "$$" 2>/dev/null || return 0
    printf '%s[update] Umbra %s is out; this copy is %s.%s\n' "$YEL" "$latest" "$mine" "$OFF"
    printf '%s         https://github.com/%s/releases/latest%s\n' "$DIM" "$REPO" "$OFF"
}
if [ "$QUIET" = 0 ]; then
    check_update &
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
