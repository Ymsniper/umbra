cmake_minimum_required(VERSION 3.16)
project(TheFinals CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Release build flags ────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-O2 -Wall -Wno-unused-variable)

# ── Sources ───────────────────────────────────────────────────────────────────
# Xlib lives in its own translation unit: X.h has `typedef XID Font`, which
# collides with raylib's Font struct, so no file may include both.
set(SOURCES
    src/main.cpp
    src/x11_overlay.cpp
    src/vmouse.cpp
    src/menu.cpp
    src/sonar.cpp
    third_party/rlImGui/rlImGui.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp)

add_executable(TheFinals ${SOURCES})

target_include_directories(TheFinals PRIVATE
    src
    third_party/imgui
    third_party/rlImGui)

# rlImGui ships an optional Font Awesome atlas we do not use.
target_compile_definitions(TheFinals PRIVATE NO_FONT_AWESOME)

# ── raylib ────────────────────────────────────────────────────────────────────
# Arch/CachyOS:   sudo pacman -S raylib
# Ubuntu/Debian:  sudo apt install libraylib-dev   (24.04+; older releases are
#                 too old, build from source instead)
# From source:
#   git clone --depth 1 https://github.com/raysan5/raylib.git && cd raylib
#   cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
#   sudo cmake --install build
find_package(raylib QUIET)
if(raylib_FOUND)
    target_link_libraries(TheFinals raylib)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(RAYLIB REQUIRED raylib)
    target_include_directories(TheFinals PRIVATE ${RAYLIB_INCLUDE_DIRS})
    target_link_libraries(TheFinals ${RAYLIB_LIBRARIES})
    message(STATUS "raylib: found through pkg-config")
endif()

# ── pthreads ──────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(TheFinals Threads::Threads m dl)

# ── X11 ───────────────────────────────────────────────────────────────────────
# The game runs on XWayland under KDE Plasma Wayland, and so does the overlay.
# X11 gives the click-through input shape, absolute positioning, and the global
# pointer and key state the overlay needs while it has no focus.
# XShape (libXext) is required, not optional: without it the overlay would sit
# on top of the game swallowing every click.
#   Arch/CachyOS:  sudo pacman -S libx11 libxext
#   Ubuntu/Debian: sudo apt install libx11-dev libxext-dev
find_package(X11 REQUIRED)
if(NOT X11_Xext_FOUND)
    message(FATAL_ERROR
        "XShape (libXext) not found. Without it the overlay blocks every mouse "
        "click. Arch: sudo pacman -S libxext   Debian: sudo apt install libxext-dev")
endif()
target_include_directories(TheFinals PRIVATE ${X11_INCLUDE_DIR})
target_link_libraries(TheFinals ${X11_LIBRARIES} ${X11_Xext_LIB})

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
