# Umbra

An external overlay for THE FINALS on Linux.

It reads the game's memory from a separate process, draws a transparent
click-through window on top of it, and provides an aim assist and a triggerbot.
Nothing is injected into the game and nothing is ever written back; the tool
reads memory and moves the mouse, and that is all.

Built from scratch on Linux: every game offset in this repository was derived by
observing the running process, not copied from a published SDK dump.






https://github.com/user-attachments/assets/858e94f2-762b-4c26-877f-fb490e66e42b




---

## Requirements

* **raylib 5.0+** for the window and drawing
* libX11 and libXext, CMake 3.16+, a C++17 compiler
* Dear ImGui and rlImGui are vendored in `third_party/`; nothing to install
* **X11 or XWayland.** Pure Wayland will not work: the overlay needs XShape for
  click-through and global hotkey polling, and Wayland deliberately gives an
  external process neither. Any desktop is fine; X11 is the requirement, not
  a particular DE.
* The game running under Proton or Wine, in **Windowed** mode rather than
  Fullscreen, so the overlay can draw over it

**Arch / CachyOS**

```bash
sudo pacman -S --needed base-devel cmake raylib libx11 libxext
```

**Ubuntu 24.04+ / Debian 13+**

```bash
sudo apt install build-essential cmake libraylib-dev libx11-dev libxext-dev
pkg-config --modversion raylib           # must be 5.0 or newer
```

Older releases package a raylib too old to build against; build it from source
if `pkg-config` reports below 5.0.

[REQUIREMENTS.md](REQUIREMENTS.md) covers the rest: building raylib from
source, kernel headers for the module, Secure Boot, `ptrace_scope`, and how to
verify a working setup.

### Tested on

```
CachyOS                kernel 7.1.2-3-cachyos, built with clang 22.1.6
KDE Plasma 6.7.2       Wayland session, overlay running through XWayland
raylib 6.0             CMake 4.3.4, GCC 16.1.1
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
./run.sh -q             # quiet: a few status lines only, no update check
```

Start it whenever you like, including at the menu. It re-resolves its objects
when a match begins or ends, so it does not need restarting between rounds.

On start it also asks GitHub whether a newer release is out, and if there is one
it says so, with a link. It never downloads or changes anything, and the tool
does not wait for the answer.

### Controls

| Key | Action |
|-----|--------|
| `INSERT` | Toggle the settings menu. While open, the overlay takes mouse clicks; while closed, clicks pass through to the game. |
| `HOME` | Toggle the aim assist. |
| `End` | Quit, with the overlay focused. `Ctrl-C` in the terminal also works. |

Settings are edited in the menu and written to `settings.cfg` a few seconds
after they stop changing, as well as on exit, so a crash or a kill costs
nothing. The file is replaced whole rather than rewritten in place, and the
previous contents are kept once as `settings.cfg.bak`.

---

## Features

### ESP
<img width="872" height="539" alt="esp" src="https://github.com/user-attachments/assets/b46d1fd6-cf1a-4f23-967e-d86d5dbbebd7" />

Boxes, skeletons, names, health bars, distance and snaplines. Squad colours
distinguish teams, and a master opacity slider governs everything drawn.

The skeleton is composed from the mesh's own bone hierarchy, so it follows the
animation rather than approximating from a capsule.

The **outline** traces each player's own silhouette in their squad's colour, so
an arm held out, a crouch or a lean shows as it is, where a box only shows how
tall someone is. It comes from the mesh the game renders, posed on the live
skeleton, when the mesh's vertices can be read, and otherwise from tubes laid
along the bones and proportioned from that player's own build. Its width, and
how much colour fills the body, are set beside it in the ESP tab.

Spectators are left out. The game lists them with the players, but they carry
no health, so a box on one reads 0 HP, and the game never draws them, so there
is nothing to see or hit. A switch in the ESP tab shows them anyway. The sonar
and the off-screen pins never do, and the aim assist and triggerbot pass over
them unless told otherwise in the Aim tab.

### Off-screen indicators
<img width="910" height="528" alt="off-screen" src="https://github.com/user-attachments/assets/5b3b7413-eb4c-40ed-85cb-982086c60dd4" />

Anyone behind you, beside you or past the edge of the screen gets a pin along
the window's edge, in their squad's colour, pointing their way. In front
of you a pin points exactly where the player projects onto your view, through
the same projection as the ESP's boxes and lines. Behind you it goes by bearing,
so someone straight behind you is straight down and the pin holds still. Each
pin holds the player's class (L, M or H), with a band round its back for their
health, the distance behind it, and a small up or down marker when they are a
floor or more above or below you. Far pins are smaller and fainter.

With snaplines drawn from the crosshair (a switch beside Snaplines in the ESP
tab), every pin sits on its own player's line. Players in nearly the same
direction get their pins nudged apart, just far enough that neither the pins
nor their distances overlap.

Ring radius starts at its top, where the pins run along the window's border.
Lower, they sit on a ring around the crosshair instead, one that flattens
against the edges of the screen where it would cross them.

Inside a set distance the pin flickers neon green, faster the closer the player
gets, while its rim keeps the squad's colour. Radius, size, range, opacity, the
flicker distance and its slowest and fastest rates are all in the menu's
Off-screen tab. Opacity sets how solid the whole indicator is, flicker and text
included, and Far opacity how much of that the farthest pins keep.

### Sonar
<img width="754" height="289" alt="sonar" src="https://github.com/user-attachments/assets/c502f090-42d2-412e-8105-202913c3caf0" />

A radar in a window of its own, so it can sit beside the game or on a second
screen rather than over the view. It turns with you: up is always the way you
are facing, and you are the marker at the centre.

* **Squad colours**, the same ones the ESP draws, so a squad reads the same in
  both. Squadmates and spectators are left out; the sonar is for what you
  cannot see.
* **Class letters** in each blip, L, M or H, or plain dots to read it at a
  glance.
* **Range** is centre to rim, in metres. Anyone past it is not drawn at all,
  so nothing piles up against the edge, and optional rings at a third and two
  thirds give the distance a scale.
* **Where you put it**: drag it anywhere, resize it from the corner, then lock
  it in place. Position, size and opacity are all remembered.

It has its own tab in the menu.

### Aim assist
<img width="797" height="702" alt="aim" src="https://github.com/user-attachments/assets/182280ef-5cde-4563-80af-490bb3216545" />

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
* **Curved pull**: approaches the target on an arc rather than a straight line,
  since a dead-straight path is a shape no hand draws. Layers over either
  smoothing mode: it sets the shape of the path, smoothing still sets the rate.
  Lateral and vertical curvature, arc direction and a per-frame jitter are all
  adjustable. Off by default. (After Witschel and Wressnegger, EuroSec 2020.)
* **Quick scope**: releases the aim the instant you fire, so a sniper's recoil
  is yours to ride rather than something the assist fights. It re-arms on the
  next ADS press, or after a delay if you set one. Needs the aim held on right
  mouse, since the shot has to be a different button from the aim.
* **Pause after a kill**: when the player the aim is pulling onto dies, the aim
  stops instead of moving on to the next closest one. It re-arms on the next
  press of the aim button, or after a delay if you set one. Off by default.

### Triggerbot
<img width="910" height="404" alt="trigger" src="https://github.com/user-attachments/assets/f3b43775-d6e3-41c6-b5f9-bb093ab83bc5" />

Fires when the shot would land. A crosshair is a direction, not a dot, so the
test is the one the game itself runs: the line from the camera through the
crosshair, against the player's body.

* **The body** is the mesh the game draws, posed on the live skeleton, when its
  vertices can be read, and otherwise tubes laid along the bones, sized from
  each player's own hip-to-head length. Either way an arm held out is where it
  is, and the gap between someone's legs is a miss.
* With the aim assist **active**, it fires only on the player the aim is pulling
  toward, and only on the part it is aiming at.
* On its **own**, it fires on whoever the line reaches first: a chosen part
  (head, chest, body or legs), or anywhere on them.
* **Forgiveness is in pixels**, which is an angle, so a setting means the same
  thing point-blank and across the map.
* **Moving players are led** by the same prediction as the aim, since the pose
  being tested is already a frame or two old.
* **Arm delay** (after the button goes down), **reaction delay** (after the
  crosshair lands), **click duration** and **cooldown** are all adjustable.

### Visibility

The engine decides what to draw every frame and stamps each mesh with the time
it last drew it, so reading that stamp answers whether a player can be seen far
better than anything computed from outside: it is the same answer the game acted
on, occlusion, culling and blown-open walls included.

* **The clock.** A stamp means nothing without the engine's own clock, which
  pauses, is dilated and restarts between rounds. It is read from the world
  itself, the clock the stamps are written from, and only believed while the two
  agree; without that offset the newest stamp anyone carries stands in for it.
* **The slack.** The game writes a stamp a frame or so after its clock has moved
  on, and the tool reads at a rate of its own, so a player counts as drawn for
  at least two of the game's own frames, measured as it runs. The tolerance
  slider can ask for more than that, never less.
* **Out of view.** Someone outside your view cannot be on your screen, so a fresh
  stamp on them was drawn for something else, their shadow for one, and they
  count as hidden.
* **Steady.** A verdict has to hold for two frames before it changes, so a
  player in a doorway does not flicker between seen and hidden.

<img width="910" height="249" alt="visibility" src="https://github.com/user-attachments/assets/5f4dfb99-9d1b-4498-93db-89cb16cc9709" />

Players who are not currently being drawn by the game are crossed out and faded,
and the aim assist and triggerbot can each be told to ignore them.

It is conservative by nature. The game draws a player whenever their bounding
box is not wholly hidden, and that box is larger than the player, so someone
just behind a corner or a low wall can still read as visible, and occlusion lags
a frame or two. The engine also keeps a second stamp meant for being drawn on
screen alone; the tool looks for it among the neighbouring fields, and the
Visibility tab says which stamp it is reading.

---

## Offsets

Every game-specific address lives in `offsets.cfg`, read at startup from the
working directory or one level above it.

The tool **refuses to start without it**, or without the few offsets nothing
works without: the controller and pawn that point at each other, the player
list, each player's pawn, where a pawn stands, and a camera (the camera
manager's POV, or the controller's rotation). It names whichever are missing.

Every other offset belongs to one feature, and an empty one turns off only that
feature. The tool lists what is off when it starts. Nothing is left silently
zero, because a zero offset still reads something: the object's vtable pointer,
which for the squad is the same on every pawn, so every player looks like your
squadmate and the screen stays empty with no explanation.

The spectator flag can hide the whole lobby when it is wrong rather than empty,
so it is checked against the match while it runs, ignored when it contradicts
it, and the log says so. When players are listed but none is drawn, the log
says what removed them, squadmates included.

Some of the offsets describe the engine's object array rather than a member:
where the module keeps its address, the two keys that unscramble it, and how its
entries are laid out. With those the tool reads its way to the player and starts
at once. Without them it falls back to scanning the game's memory for the same
thing, which works but costs seconds at every start and again after each match.

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
gobjects.code         the game's own instructions that reach the object array
CMakeLists.txt
src/
  main.cpp              entry point, overlay window, render loop
  menu.cpp              the settings window, a process of its own
  sonar.cpp             the sonar window, a process of its own
  shared.hpp            settings and status shared with those two
  mem.hpp               process memory reads
  cheat.hpp             reader thread, entity list
  render.hpp            ESP drawing, aim assist, triggerbot
  offscreen.hpp         off-screen indicators
  outline.hpp           the outline: the body's mask and the edge traced round it
  body.hpp              a player's mesh, read once and posed on the live bones
  hitbox.hpp            the shot's line against tubes along the bones
  visibility.hpp        render stamps, the engine's clock, the view test
  global.hpp            state shared between reader and render threads
  structs.hpp           engine types and world-to-screen projection
  skeleton.hpp          bone hierarchy composition
  settings.hpp          settings.cfg load and save
  vmouse.hpp, .cpp      mouse output
  x11_overlay.hpp, .cpp the overlay window and input under X11
  colors.hpp, font.hpp  drawing helpers
  offsets.hpp           fixed engine layout constants
  runtime_offsets.hpp   offsets.cfg loader
  gobjects_direct.hpp   object array decoding
  gobjemu.hpp           runs the instructions in gobjects.code
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
`Discovery.exe` (`Discovery-d.exe` on older builds) and the correct thread is
`GameThread`.

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
