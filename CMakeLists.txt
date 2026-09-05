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
set(SOURCES src/main.cpp)

add_executable(TheFinals ${SOURCES})

target_include_directories(TheFinals PRIVATE src)

# ── SFML 3 ────────────────────────────────────────────────────────────────────
# Requires SFML 3.x - the 2.x API was removed.
# Install: sudo apt install libsfml-dev   (Ubuntu 24.04+ ships SFML 3)
# OR build from source:
#   git clone --depth 1 https://github.com/SFML/SFML.git && cd SFML
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && sudo cmake --install build
find_package(SFML 3 COMPONENTS Graphics Window System REQUIRED)
target_link_libraries(TheFinals SFML::Graphics SFML::Window SFML::System)

# ── ImGui-SFML (must be the SFML-3-compatible version) ────────────────────────
# Install:
#   git clone --depth 1 https://github.com/SFML/imgui-sfml.git
#   cd imgui-sfml
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && sudo cmake --install build
# OR: vcpkg install imgui-sfml
if(DEFINED IMGUI_SFML_DIR)
    target_include_directories(TheFinals PRIVATE ${IMGUI_SFML_DIR}/include)
    target_link_directories(TheFinals PRIVATE ${IMGUI_SFML_DIR}/lib)
endif()
find_package(ImGui-SFML QUIET)
if(ImGui-SFML_FOUND)
    target_link_libraries(TheFinals ImGui-SFML::ImGui-SFML)
else()
    # Fallback: link by name (set IMGUI_SFML_DIR above if CMake can't find it)
    target_link_libraries(TheFinals ImGui-SFML)
    message(STATUS "ImGui-SFML: linking by name (set IMGUI_SFML_DIR if not found)")
endif()

# ── pthreads ──────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(TheFinals Threads::Threads)

# ── X11 (for click-through overlay on XWayland) ───────────────────────────────
# On KDE Plasma Wayland the game runs on XWayland; this overlay also runs on
# XWayland (SFML's X11 backend). X11 gives us XShapeCombineRegion for
# click-through and correct setPosition() behaviour.
# Force X11 mode at runtime: DISPLAY=:0 WAYLAND_DISPLAY="" ./TheFinals
find_package(X11 QUIET)
if(X11_FOUND)
    target_compile_definitions(TheFinals PRIVATE HAVE_X11)
    target_link_libraries(TheFinals ${X11_LIBRARIES})
    target_include_directories(TheFinals PRIVATE ${X11_INCLUDE_DIR})

    # XShape (libXext) - required for click-through overlay on XWayland
    # Arch/CachyOS: sudo pacman -S libxext
    # Ubuntu/Debian: sudo apt install libxext-dev
    if(X11_Xext_FOUND)
        target_compile_definitions(TheFinals PRIVATE HAVE_XSHAPE)
        target_link_libraries(TheFinals ${X11_Xext_LIB})
        message(STATUS "XShape click-through: enabled")
    else()
        message(WARNING "XShape (libXext) not found - overlay will BLOCK mouse clicks! Install libxext and rebuild.")
    endif()
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "X11 found: ${X11_FOUND}")
