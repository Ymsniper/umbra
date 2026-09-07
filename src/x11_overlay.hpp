#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// The X11 side of the overlay, behind a plain interface.
//
// Xlib is kept in its own translation unit on purpose: X.h has `typedef XID
// Font`, which collides with raylib's `Font` struct, and Xlib.h defines None,
// True, False, Bool and Status as macros. Nothing that includes raylib can
// include Xlib, so nothing here exposes an Xlib type.
#include <sys/types.h>

namespace ovl {

// Opens the X connection used for window properties, geometry and input.
// Returns false when there is no display, in which case every other call is
// a no-op and the overlay runs without click-through or positioning.
bool init();
void shutdown();

// The game's window rect, located by process id. False until it is mapped.
bool gameRect(pid_t gamePid, int& x, int& y, int& w, int& h);

// Finds the X window this process created (raylib's) and turns it into an
// overlay: click-through, always on top, no taskbar entry, no WM stacking.
bool adoptOwnWindow(const char* title);

// Absolute position, corrected for a window manager frame if there is one.
void moveResize(int x, int y, int w, int h);
void raise();

void setClickThrough(bool through);

// The overlay runs unmanaged so it never takes focus from the game, which also
// means the menu cannot be clicked, scrolled or typed into. Menu mode turns it
// back into an ordinary focusable window for as long as the menu is up; leaving
// it restores the click-through overlay and hands focus back to the game.
void setMenuMode(bool on, pid_t gamePid);

// Global input state. The overlay never holds focus, so the game's clicks and
// keys never reach its event queue; these read the server directly instead.
bool lmbDown();
bool rmbDown();
bool keyDown(int key);          // see Key below

// Pointer position in overlay-local pixels, with the two buttons. Queried
// rather than taken from an event, so it is unaffected by which client holds a
// pointer grab and by the overlay never being focused.
bool pointer(int& x, int& y, bool& lmb, bool& rmb);

enum Key { KeyHome = 0, KeyInsert, KeyEnd };

}  // namespace ovl
