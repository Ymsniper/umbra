#!/usr/bin/env bash
# Build Umbra. Requires SFML 3, ImGui-SFML and X11 development packages.
#   ./build.sh            configure (if needed) and build
#   ./build.sh clean      start from a fresh build directory
set -euo pipefail
cd "$(dirname "$0")"

if [ "${1:-}" = "clean" ]; then
    rm -rf build
fi

# CMake records the absolute source path in its cache, so a build directory
# stops working the moment the project is moved or renamed. The error it gives
# is not obvious, and the fix is always the same, so do it here instead.
if [ -f build/CMakeCache.txt ]; then
    cached=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' build/CMakeCache.txt)
    if [ -n "$cached" ] && [ "$cached" != "$PWD" ]; then
        echo "build/ was configured for:"
        echo "    $cached"
        echo "  but the project is now at:"
        echo "    $PWD"
        echo "  discarding the stale build directory and reconfiguring."
        echo
        rm -rf build
    fi
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

echo
echo "Built build/TheFinals"
echo

# Read /proc/modules rather than piping lsmod into grep: under `set -o pipefail`
# a matching `grep -q` exits early, lsmod dies of SIGPIPE, and the pipeline
# reports failure exactly when the match succeeded.
if grep -q '^suite_kmod ' /proc/modules 2>/dev/null; then
    echo "Kernel module is already loaded; reads and mouse input will use it."
    echo
    echo "  ./run.sh"
    exit 0
fi

cat <<'MSG'
Optional kernel module
  Reads memory from kernel space, so ptrace_scope does not apply and the tool
  needs no sudo, and injects mouse input into the real pointer instead of a
  uinput device, so nothing extra appears in /proc/bus/input/devices.

      cd kmod && make
      sudo insmod suite_kmod.ko

  It must be loaded BEFORE the tool starts; the backend is chosen once.

Either way, start with:

      ./run.sh

MSG

# Only offer when someone is actually at a terminal, so scripted builds and CI
# are unaffected.
if [ -t 0 ] && [ -t 1 ]; then
    read -r -p "Build and load the kernel module now? [y/N] " reply
    case "$reply" in
        [Yy]*)
            echo
            if ! ( cd kmod && make ); then
                echo
                echo "Module build failed. The tool still works without it."
                exit 0
            fi
            echo
            if sudo insmod kmod/suite_kmod.ko; then
                echo "Loaded. Start the tool with ./run.sh"
            else
                echo "insmod failed. Already loaded, or Secure Boot is blocking it."
                echo "The tool still works without it."
            fi
            ;;
        *)
            echo "Skipped. ./run.sh works without it."
            ;;
    esac
fi
