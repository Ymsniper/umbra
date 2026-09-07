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

// Used by the MENU process, whose window is an ordinary managed one. Showing it
// lets the window manager activate it, which is what makes the game release the
// pointer; hiding it hands activation back the way closing any window does.
bool menuAdopt(const char* title);
void menuSetVisible(bool on);

// Hands the game back the keyboard and, more importantly, the pointer. Wine
// re-applies its cursor clip when its window is ACTIVATED, which is a window
// manager path that setting the input focus directly does not go through, so
// without this the cursor stays loose and wanders off the game window.
void focusGame(pid_t gamePid);

// Minimise the game and bring it straight back. A game confines the cursor
// through Wine's ClipCursor, which is an X pointer grab; that grab is lost
// while the tool starts and Wine never notices, because it ignores
// NotifyGrab/NotifyUngrab. The game re-applies the clip only when it is
// deactivated and activated again, and asking a window manager to activate the
// window it already considers active does nothing -- so it takes a real
// minimise and restore, done here instead of by hand.
void cycleGameWindow(pid_t gamePid);

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
