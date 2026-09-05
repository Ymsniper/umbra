# Requirements

Everything Umbra needs, why it needs it, and how to install it.

---

## At a glance

| Component | Needed for | Notes |
|-----------|-----------|-------|
| C++17 compiler, CMake ≥ 3.16 | building | GCC or Clang |
| **SFML 3.x** | window and drawing | **not** SFML 2; the 2.x API was removed |
| **ImGui-SFML** | the settings menu | must be built against SFML 3 |
| libX11 | overlay positioning, hotkeys | |
| libXext (XShape) | click-through | without it the overlay **blocks your clicks** |
| pthreads | reader thread | part of glibc |
| Kernel headers | the optional kernel module | toolchain is auto-detected |
| X11 or XWayland | running | pure Wayland will not work (see below) |
| Proton / Wine | running the game | game in Windowed mode, not Fullscreen |

---

## Install

### Arch, CachyOS, EndeavourOS

```bash
sudo pacman -S --needed base-devel cmake sfml libx11 libxext
```

Arch ships SFML 3 as `sfml`. `imgui-sfml` is in the AUR:

```bash
paru -S imgui-sfml        # or: yay -S imgui-sfml
```

### Ubuntu 24.04+, Debian 13+

```bash
sudo apt install build-essential cmake libsfml-dev libx11-dev libxext-dev
```

Check you actually got SFML 3, because older releases package 2.x:

```bash
pkg-config --modversion sfml-graphics     # must start with 3
```

If it reports 2.x, build SFML 3 from source (below).

ImGui-SFML is not packaged on Debian or Ubuntu; build it from source (below).

### Fedora

```bash
sudo dnf install gcc-c++ cmake SFML-devel libX11-devel libXext-devel
```

Same caveat: confirm SFML is 3.x, and build ImGui-SFML from source.

---

## Building SFML 3 from source

Only needed if your distro ships SFML 2.

```bash
git clone --depth 1 https://github.com/SFML/SFML.git
cd SFML
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build
```

## Building ImGui-SFML from source

Required on most distros. It must be compiled against the SFML 3 you installed.

```bash
git clone --depth 1 https://github.com/SFML/imgui-sfml.git
cd imgui-sfml
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build
```

If CMake still cannot find it, point the build at it directly:

```bash
cmake -S . -B build -DIMGUI_SFML_DIR=/path/to/imgui-sfml/install
```

---

## Kernel module (optional)

The tool runs without it. It provides `ptrace_scope`-independent memory reads and
mouse injection through the real pointer instead of a virtual uinput device.

You need headers for the kernel you are **currently running**:

```bash
# Arch / CachyOS
sudo pacman -S --needed linux-headers        # or linux-cachyos-headers, etc.

# Ubuntu / Debian
sudo apt install linux-headers-$(uname -r)

# Fedora
sudo dnf install kernel-devel-$(uname -r)
```

Then:

```bash
cd kmod && make
sudo insmod suite_kmod.ko
```

### Compiler must match the kernel, handled automatically

A module has to be built with the same toolchain as the kernel it loads into.
Most distributions use GCC; CachyOS and some others build with clang, which
needs `LLVM=1`.

You do not have to know which. The kernel records the compiler it was built with
in its own `.config` (`CONFIG_CC_IS_CLANG`), and the Makefile reads it, so plain
`make` is right either way. It prints what it chose:

```
  kernel   7.1.2-3-cachyos
  toolchain clang (LLVM=1)
```

Override with `make LLVM=0` or `make LLVM=1` if the detection is ever wrong.

If the headers are missing, the build stops with the install command for your
distribution rather than the kernel build system's rather opaque error.

### Secure Boot

An unsigned module will not load with Secure Boot enabled. Either sign it, or
disable Secure Boot, or skip the module; the tool falls back to
`process_vm_readv` on its own.

---

## Runtime

### X11 is required

The overlay uses **XShape** for click-through and **XQueryPointer/XQueryKeymap**
for global hotkeys. Wayland has no equivalent that an external process may use:
it deliberately forbids one client positioning itself over another or reading
global input. This is a design decision in Wayland, not a missing feature.

| Session | Works |
|---------|-------|
| Native X11, any desktop (GNOME, KDE, XFCE, i3, …) | yes |
| Wayland **with XWayland** (KDE Plasma, GNOME, Hyprland, …) | yes |
| Wayland with no XWayland | no |

On a Wayland session, force the X11 backend:

```bash
DISPLAY=:0 WAYLAND_DISPLAY= ./run.sh
```

Nothing here is tied to a particular desktop environment. KDE Plasma is what it
was developed on, nothing more.

### The game must be in Windowed mode

Set the game to **Windowed**. In Fullscreen it owns the display and nothing may
draw over it, so the overlay runs correctly and stays invisible.

The overlay locates the game's window and matches its position and size,
rechecking periodically, so it follows the window and adapts to any resolution.
This log line confirms it:

```
[x11] game window at 0,0 2560x1440 - overlay will match
```

If it instead says `game window not found`, the overlay falls back to 0,0 at its
default size and will very likely be misplaced.

### Fonts

Text labels look for DejaVu, Noto, then Liberation, in both Arch and
Debian-style paths. If none are present the tool still runs, just without text:

```bash
sudo pacman -S ttf-dejavu        # Arch
sudo apt install fonts-dejavu    # Debian/Ubuntu
```

### Memory access without the kernel module

`process_vm_readv` is subject to `ptrace_scope`. If reads fail, either load the
kernel module, or run the tool as root, or lower the restriction:

```bash
sudo sysctl kernel.yama.ptrace_scope=0     # until reboot
```

The kernel module is the better answer; it avoids loosening this system-wide.

---

## Verifying

```bash
cmake --version                          # >= 3.16
pkg-config --modversion sfml-graphics    # 3.x
ls /usr/lib/libImGui-SFML* 2>/dev/null || echo "ImGui-SFML: build from source"
ls /lib/modules/$(uname -r)/build        # kernel headers, for the module
echo "$XDG_SESSION_TYPE"                 # x11, or wayland (then use XWayland)
```

A successful configure prints:

```
-- XShape click-through: enabled
-- X11 found: TRUE
```

If it says XShape was **not** found, install `libxext` and rebuild; otherwise
the overlay will swallow your mouse clicks.
