# Umbra

An external overlay for THE FINALS on Linux.

It reads the game's memory from a separate process, draws a transparent
click-through window on top of it, and provides an aim assist and a triggerbot.
Nothing is injected into the game and nothing is ever written back; the tool
reads memory and moves the mouse, and that is all.

Built from scratch on Linux: every game offset in this repository was derived by
observing the running process, not copied from a published SDK dump.


https://github.com/user-attachments/assets/674baa52-3006-4767-a263-29178bed3a31


---

## Requirements

* **SFML 3.x**: not 2.x, the old API was removed
* **ImGui-SFML**, built against SFML 3
* libX11 and libXext, CMake 3.16+, a C++17 compiler
* **X11 or XWayland.** Pure Wayland will not work: the overlay needs XShape for
  click-through and global hotkey polling, and Wayland deliberately gives an
  external process neither. Any desktop is fine; X11 is the requirement, not
  a particular DE.
* The game running under Proton or Wine, in **Windowed** mode rather than
  Fullscreen, so the overlay can draw over it

**Arch / CachyOS**

```bash
sudo pacman -S --needed base-devel cmake sfml libx11 libxext
paru -S imgui-sfml
```

**Ubuntu 24.04+ / Debian 13+**

```bash
sudo apt install build-essential cmake libsfml-dev libx11-dev libxext-dev
pkg-config --modversion sfml-graphics    # must be 3.x
```

ImGui-SFML is not packaged there and has to be built from source.

[REQUIREMENTS.md](REQUIREMENTS.md) covers the rest: building SFML 3 and
ImGui-SFML from source, kernel headers for the module, Secure Boot,
`ptrace_scope`, and how to verify a working setup.

### Tested on

```
CachyOS                kernel 7.1.2-3-cachyos, built with clang 22.1.6
KDE Plasma 6.7.2       Wayland session, overlay running through XWayland
SFML 3.1.0             CMake 4.3.4, GCC 16.1.1
Intel UHD + RTX 4060   game under Proton, Windowed
```

Two parts of that are worth calling out, because they are the awkward cases:

* **It is a Wayland session.** The overlay does not run on Wayland natively and
  it does not need to: the game runs under XWayland and so does the overlay, and
  X11 click-through and hotkeys work normally inside it. If you are on Wayland,
  this is the configuration that works.
* **The kernel is clang-built**, which is unusual and is what the module's
  toolchain detection exists for. On a GCC kernel the module takes the other
  branch, which is the common case but the less exercised one here.

This is the development machine, so it is the only configuration actually
verified. Other distributions, desktops and GCC-built kernels are expected to
work and are untested.

---

## Build

```bash
chmod +x build.sh run.sh   # a zip download drops the executable bit
./build.sh                 # configure and build
./build.sh clean           # from a fresh build directory
```

The binary is written to `build/TheFinals`.

CMake stores the absolute source path in its cache, so a build directory
carried across a move or rename stops working. `build.sh` notices and
reconfigures on its own, so moving the project is not something you have to
think about.

---

## Kernel module (optional, stealthier)

`kmod/` builds a small module that makes the tool considerably quieter on the
system. It is entirely optional: without it everything still works, using the
userspace fallbacks noted below.

* **Memory reads** go through `access_process_vm()` in kernel space, so
  `ptrace_scope` does not apply. The tool needs no `sudo`, nothing has to attach
  to the game, and yama does not have to be loosened for the whole system.
* **Mouse input** is injected into the real pointer instead of a virtual uinput
  device. Without the module the tool must create one, and a virtual input
  device is enumerable: it shows up in `/proc/bus/input/devices` for anything
  that cares to look. With the module there is no extra device at all.

```bash
cd kmod && make
sudo insmod suite_kmod.ko
```

A module must be built with the same compiler as the kernel it loads into. The
Makefile reads that from the kernel's own config and sets the toolchain itself,
so plain `make` is correct on a GCC kernel and on a clang one alike:

```
  kernel   7.1.2-3-cachyos
  toolchain clang (LLVM=1)
```

Override with `make LLVM=0` or `make LLVM=1` if it ever guesses wrong. If the
kernel headers are missing it says so and prints the install command for your
distribution.

With Secure Boot enabled an unsigned module will not load: sign it, disable
Secure Boot, or simply skip the module.

Load it **before** starting the tool; the backend is chosen once, at startup.

Confirm which backend is live in the menu, or in the log:

```
[mem] kernel backend: /dev/suite_kmod
[vmouse] kernel injection into the real pointer
```

Unload with `sudo rmmod suite_kmod`.

---

## Run

**Set the game to Windowed mode first.** In Fullscreen the game owns the display
outright and nothing can draw over it, so the overlay will be running correctly
and still be invisible.

The overlay finds the game's window and matches its position and size,
rechecking as you go, so it follows the window if you move or resize it, and
adapts to any resolution.

```bash
./run.sh                # finds the game PID by itself
./run.sh 12345          # or give it one
./run.sh -q             # quiet: a few status lines only
```

Start it whenever you like, including at the menu. It re-resolves its objects
when a match begins or ends, so it does not need restarting between rounds.

### Controls

| Key | Action |
|-----|--------|
| `INSERT` | Toggle the settings menu. While open, the overlay takes mouse clicks; while closed, clicks pass through to the game. |
| `HOME` | Toggle the aim assist. |
| `End` | Quit, with the overlay focused. `Ctrl-C` in the terminal also works. |

Settings are edited in the menu and saved to `settings.cfg` when the tool exits,
so it comes back the way you left it.

---

## Features

### ESP

Boxes, skeletons, names, health bars, distance and snaplines. Squad colours
distinguish teams, and a master opacity slider governs everything drawn.

The skeleton is composed from the mesh's own bone hierarchy, so it follows the
animation rather than approximating from a capsule.

### Aim assist

Pulls toward a target while the chosen mouse button is held.

* **Target selection**: a bias slider between "closest to me" and "closest to
  the crosshair", so you can take the player behind the nearest one by pointing
  at him.
* **Smoothing**: a divisor mode and an inertia (EMA) mode.
* **Stickiness**: keeps the current target unless a challenger is clearly
  better, instead of hopping between two enemies at similar angles.
* **Bone selection**: head, chest, body or legs, aimed at the real joint when
  the skeleton resolves.
* **Prediction**: leads a moving target by its velocity. Without this the aim
  trails a moving head by a constant amount, because the position it was given
  is already a frame old by the time the mouse moves.

### Triggerbot

Fires when the crosshair is on target.

* With the aim assist **active**, it fires only when the crosshair has reached
  the exact point the aim is pulling toward.
* On its **own**, it works from the skeleton (a chosen bone, or any
  bone), or from the capsule box.
* **Tolerance scales with the target's on-screen size**, so one setting behaves
  the same point-blank and across the map. A fixed pixel tolerance cannot: at
  range a few pixels span a whole head, up close they are a sliver of one.
* **Arm delay** (after the button goes down), **reaction delay** (after the
  crosshair lands), **click duration** and **cooldown** are all adjustable.

### Visibility

Players who are not currently being drawn by the game are crossed out and faded,
and the aim assist and triggerbot can each be told to ignore them.

This is derived from the engine's own render timestamp, so it is conservative by
nature: the game culls on a bounding box, which is larger than the player, and
occlusion queries lag a frame or two. Expect an enemy to be marked visible
slightly before he fully clears a corner. The tolerance slider controls how
quickly someone drops out of visible after breaking line of sight.

---

## Offsets

Every game-specific address lives in `offsets.cfg`, read at startup from the
working directory or one level above it.

The tool **refuses to start without it** and names anything missing. This is
deliberate: an offset that is silently zero produces an empty screen with no
explanation, which is far worse to diagnose than a clear failure.

A game update moves these. When that happens the tool starts but finds nothing.
Re-derive the offsets and copy the new `offsets.cfg` here. No rebuild is needed.

---

## Layout

```
README.md             this file
REQUIREMENTS.md       dependencies and how to install them
LICENSE               GPL-2.0
build.sh              build script
run.sh                launcher
banner.txt            startup banner
offsets.cfg           game offsets (required)
CMakeLists.txt
src/
  main.cpp              entry point, overlay window, render loop
  mem.hpp               process memory reads
  cheat.hpp             reader thread, entity list
  render.hpp            ESP drawing, aim assist, triggerbot, menu
  global.hpp            state shared between reader and render threads
  structs.hpp           engine types and world-to-screen projection
  skeleton.hpp          bone hierarchy composition
  settings.hpp          settings.cfg load and save
  vmouse.hpp            mouse output
  offsets.hpp           fixed engine layout constants
  runtime_offsets.hpp   offsets.cfg loader
  gobjects_direct.hpp   object array decoding
kmod/
  suite_kmod.c          kernel module: memory reads and mouse injection
  suite_kmod.h          shared ioctl contract
  Makefile
```

`settings.cfg` is created the first time the tool exits.

---

## Troubleshooting

**`offsets.cfg not found`**: run from the project directory, or keep the file
beside the binary.

**`offsets.cfg is present but incomplete`**: the offsets it names are zero. The
game has most likely updated; re-derive them.

**Nothing appears on screen, but the log looks healthy**: the game is probably in
Fullscreen, which lets nothing draw over it. Switch it to Windowed mode. The
log line `game window at X,Y WxH - overlay will match` confirms the
overlay found and matched the window.

**The overlay eats your mouse clicks**: libXext was missing when you built, so
click-through is compiled out. The configure step warns about this; look for
`XShape click-through: enabled`. Install `libxext`/`libxext-dev` and rebuild.

**Overlay in the wrong place, or clicks not passing through**: the overlay needs
X11. Under Wayland, force XWayland:

```bash
DISPLAY=:0 WAYLAND_DISPLAY= ./run.sh
```

**Game not found**: pass the PID directly. Under Proton the process is
`Discovery-d.exe` and the correct thread is `GameThread`.

**Nothing drawn during a match**: offsets are stale after a game update.

**Aim assist does nothing**: check that the menu reports a mouse backend. If
"visible only" is enabled and every enemy is behind cover, there is deliberately
no target.

**Kernel module will not build**: it must be compiled with the same toolchain as
your running kernel. On a clang-built kernel: `make LLVM=1`.

---

## A note on risk

This reads another process's memory and injects mouse input. It does not modify
the game, but using it in an online match is against the game's terms of service
and can cost you the account. That is your decision to make; make it knowingly.

---

## License

GNU General Public License, version 2. See [LICENSE](LICENSE).

You may use, study, modify and redistribute this. If you distribute a modified
version you must ship its source under the same licence, so everyone who
receives it keeps the same freedoms. There is no warranty.

GPLv2 rather than v3 deliberately: the kernel module in `kmod/` is a Linux
kernel module, the kernel is GPL-2.0-only, and `MODULE_LICENSE("GPL")` means
version 2. Licensing the whole project the same way keeps the userspace tool and
the module compatible with each other and with the kernel, with no split to
reason about.

Copyright © 2026 Ymsniper.
