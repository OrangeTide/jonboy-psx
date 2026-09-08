#ifndef SCRIPT_H
#define SCRIPT_H

#include <stdint.h>

// Timed system-event script. A text script lists events by video-frame number,
// clocked by the VBLANK frame counter in sched.c. It drives the machine without a
// human, for demos, regression captures, and headless control (booting a menu
// entry, swapping discs). Pad input is one event type among several, so the
// script is not limited to the controller.
//
// Line format: "<frame> <command> <args...>". Commands:
//   pad <tokens...>   Set the controller state held from this frame on. Tokens
//                     are button names (up, down, left, right, cross, circle,
//                     square, triangle, start, select, l1, r1, l2, r2, l3, r3),
//                     "none" to release all, or stick settings lx=, ly=, rx=, ry=
//                     with a 0..255 value. A pad keyframe fully specifies the
//                     state: buttons not named are released, sticks not set are
//                     centered (0x80).
//   analog on|off     Set the DualShock analog mode.
//   disc <path>       Insert or change the disc (mount the image).
//   disc eject        Remove the disc.
// Blank lines and lines beginning with # are ignored. Pad keyframes are held
// until the next pad keyframe; the other events fire once when their frame is
// reached. If the frame counter restarts (a reboot), the script replays.

// Load a script file. Returns 0 on success, -1 on failure (no script active).
int script_load(const char *path);

// True if a script is loaded and driving the machine.
int script_active(void);

// Advance the script to `frame`: apply the active pad keyframe and fire any
// one-shot events now due. Called once per emulated video frame; a no-op when no
// script is loaded.
void script_frame(uint32_t frame);

#endif
