# Native menu overlay

A host-side (SDL_Renderer) menu overlay for the windowed build. It suspends the
running game, draws a menu bar and modal dialogs over the last frame, and resumes
in place. It does not run on the emulated console, so it needs no save-state to
show the menu: the console globals are simply frozen while the main thread draws.

This is the Option A design (engine-native draw, not a vendored toolkit). The
drawing primitives are plain SDL_Renderer calls plus a small bitmap font, so the
overlay composites directly onto the existing 2D present path.

## Model

- The overlay is modal over the game but non-destructive: opening it sets a pause
  flag the emulation thread honors, and the frozen VRAM/RAM/registers are left
  untouched.
- Two layers: a persistent **menu bar** across the top, and at most one **modal
  dialog** centered over a dimmed game frame.
- Everything is reachable by keyboard, gamepad, and mouse. A controller-only user
  can drive the entire UI.

### Pause and present harness

The structural wiring (independent of what is drawn):

1. `SDL_AtomicInt app.paused`. The emu thread's step loop checks it and idles
   (keeps the thread alive, stops stepping). SPU drains and the audio callback
   pads silence; the GPU worker parks. Nothing is serialized.
2. A hotkey (default `Esc`, or `F2`) toggles `app.overlay`. While open, input is
   routed to the UI and `update_pad_input` is skipped so the game sees no input.
3. `present_frame` is refactored to render the game frame and return, with no
   `SDL_RenderPresent` inside it. The main loop then draws the overlay (dim quad
   plus UI) when open and calls `SDL_RenderPresent` once. This single-present
   change is the only edit to existing render code.
4. Closing the overlay clears `app.paused`; the core resumes from the exact
   instruction it froze on.

Opening the menu does not require a game to be running. With no game it is the
front end: the dimmed layer is just black and File is the entry point.

## Menu bar

Horizontal bar, left aligned, with keyboard mnemonics and gamepad navigation
(L1/R1 cycle menus, D-pad moves, Cross/Enter opens, Circle/Esc closes).

```
 File   State   Cards   Settings   View   Help
```

### File
- Open Disc...            file picker, then `cdrom_mount` (hot swap, raises lid INT5)
- Boot Game...            pick a disc/EXE, full reset, then boot it
- Recent               >  MRU list from config, each boots on select
- Close Disc             `cdrom_eject`
- Swap Disc...           for multi-disc titles, same as Open Disc
- --
- Select BIOS...         file picker, stored in config, applied on next reset
- --
- Reset                  soft reset the console
- Power Off              tear the core down to the no-game front end
- Exit                   quit the application

### State
- Save State           >  slots 1..9, plus Quick (F5)
- Load State           >  slots 1..9, plus Quick (F9)
- Save State to File...
- Load State from File...
- Undo Last Load

Save/Load State depend on a serialization layer that does not exist yet (see
"Core plumbing"). The menu is the driver; the slots are inert until that lands.
Grey the items out and show "unavailable" until then.

### Cards
- Slot 1               >  shows current file; Insert..., Eject, New...
- Slot 2               >  same
- Card Manager...        optional later: browse blocks/saves on a card

Insert maps to `mc_mount_slot(slot, path)`; New creates and formats a 128 KB
image at a chosen path then mounts it; Eject detaches (clears the slot).

### Settings
Opens the tabbed Settings dialog (below). Also a flat path for quick toggles:
- Fullscreen            (also F11)
- Pause on Focus Loss   (checkbox)
- Performance HUD       (toggles the JONBOY_PERF overlay line)

### View
- Aspect             >  4:3, Stretch, Integer
- Scale              >  1x, 2x, 3x, Fit
- VRAM View            (F1, the existing full-VRAM debug view)
- Fullscreen           (F11)

### Help
- Controls / Keys...     the keybind and gamepad reference dialog
- About...               version, build, credits

## Widget kit (minimal)

Option A needs only a small set of primitives, drawn with `SDL_RenderFillRect`,
`SDL_RenderRect`, `SDL_RenderTexture` (for the game frame and any icons), and text
via `SDL_RenderDebugText` or the existing 8x8 font. One focusable "control" struct
with a type tag and a rect, laid out in a vertical stack per dialog:

- label             static text
- button            text, on-activate callback
- checkbox          bool, toggles on activate
- slider            int/float range, left/right adjusts, shows value
- spinner           discrete int (e.g. FASTTICK N), left/right steps
- dropdown          enumerated choice, opens a small popup list
- textfield         path or name entry (keyboard), with a "Browse..." button
- keycapture        waits for the next key/button press and binds it
- list              scrollable selectable rows (recent, slots, file browser)
- tabstrip          switches dialog pages (L1/R1)
- modal frame       title bar, body, button row (OK / Cancel / Apply)

No immediate-mode retained tree, no layout engine: each dialog is a fixed array of
controls with hand-placed rows. That is enough for every dialog below.

## Dialogs

All dialogs are centered, sized to content, over a 60% black dim of the game
frame. Tab/D-pad moves focus, Cross/Enter activates, Circle/Esc cancels, L1/R1
switches tabs. Mouse hover focuses and click activates.

### File picker

Prefer SDL3's native `SDL_ShowOpenFileDialog` on desktop (zero UI to build, native
look, remembers last dir). Provide an in-window fallback browser for
controller-only or kiosk use:

```
+-- Open Disc ---------------------------------+
| Path: /home/jon/DEVEL/jonboy/REF/games/      |
| +------------------------------------------+ |
| | ..                                       | |
| | Chrono Trigger (USA).cue                 | |
| | Final Fantasy Tactics (USA).cue       <- | |
| | Lunar (Disc 1).chd                       | |
| +------------------------------------------+ |
| Filter: *.cue *.bin *.iso *.chd *.exe        |
|                          [ Open ] [ Cancel ] |
+----------------------------------------------+
```

Directory scan reuses the loader's approach. Filter is per invocation (discs vs
BIOS vs state files).

### Save / Load State

A slot grid with a thumbnail (reuse the published framebuffer as the preview) plus
timestamp. Same layout for save and load; save writes the focused slot, load reads
it.

```
+-- Load State --------------------------------+
|  [1] 12:04  FFT        [2] 12:40  FFT battle |
|  [3] --empty--         [4] 11:58  CT         |
|  [5] --empty--         [6] --empty--         |
|  [7] --empty--         [8] --empty--         |
|  [9] --empty--      [Quick] 12:41            |
|                                              |
|  Slot 2: Final Fantasy Tactics, 12:40:17     |
|                          [ Load ] [ Cancel ] |
+----------------------------------------------+
```

State files live in a per-title directory keyed by the disc's serial or filename,
so slots do not collide across games. Each file carries a magic, a format version,
and the title id; loading rejects a mismatched version rather than corrupting the
core.

### Settings (tabbed)

```
+-- Settings -------------------------------------------+
| [ Video ] [ Audio ] [ Input ] [ Advanced ]           |
|                                                       |
|  Aspect ratio      ( 4:3  )  < >                      |
|  Window scale      ( 2x   )  < >                      |
|  Scanlines         [x]                                |
|  Texture filter    ( Nearest )  < >                   |
|  Start fullscreen  [ ]                                |
|  Pause on focus loss [x]                              |
|                                                       |
|                       [ OK ]  [ Apply ]  [ Cancel ]   |
+-------------------------------------------------------+
```

- **Video:** aspect (4:3 / stretch / integer), scale, scanlines, nearest/linear
  filter, fullscreen, performance HUD.
- **Audio:** master volume, mute, ring latency (buffer size), show underruns.
- **Input:** port 1/2 device (Digital, DualShock, None), analog-by-default,
  deadzone, and a button that opens Controller Mapping.
- **Advanced:** GPU worker on/off (`JONBOY_GPU_THREAD`), FASTTICK N spinner,
  region (NTSC/PAL/auto), BIOS path, fast boot. These mirror today's env vars and
  flags, now persisted in config.

Apply writes live where safe (volume, aspect, filter, GPU worker); settings that
only take effect on reset (BIOS, region) are marked and applied on the next reset.

### Controller Mapping

A two-column table: each PSX button and its current keyboard key and gamepad
binding. Selecting a cell enters keycapture: the next key or pad button pressed
becomes the binding. This replaces the hardcoded `pad_bit_for_key` switch with a
mutable `SDLK -> PAD_BTN` table (and an `SDL_GamepadButton -> PAD_BTN` table),
loaded from and saved to config.

```
+-- Controller Mapping (Port 1) -----------------------+
|  PSX Button     Keyboard      Gamepad                |
|  Up             Up            DPad Up                 |
|  Down           Down          DPad Down               |
|  ...                                                  |
|  Cross          Z             A              <press>  |
|  Circle         X             B                       |
|  Square         A             X                       |
|  Triangle       S             Y                       |
|  L1 / R1        Q / W         LB / RB                 |
|  L2 / R2        E / R         LT / RT                 |
|  Start/Select   Enter / RShift  Start / Back          |
|  Analog toggle  Tab           Guide                   |
|                                                       |
|  [ Rebind ]  [ Reset to Defaults ]  [ Done ]          |
+------------------------------------------------------+
```

Port selector (1/2) at the top. "Reset to Defaults" restores the current built-in
map. A conflict (same key on two buttons) warns inline.

### About

```
+-- About JonBoy ------------------------------+
|   JonBoy                                     |
|   PlayStation 1 (R3000A) emulator in C       |
|   Version <git describe>  (<commit>)         |
|   Built <date>, SDL <runtime version>        |
|                                              |
|   github.com/<...>                           |
|                                [ Close ]     |
+----------------------------------------------+
```

Version string from `git describe` baked in at build time (a `version.h` the
Makefile generates), SDL version from `SDL_GetVersion`.

### Help / Controls

A scrollable reference of the current keybinds and the menu hotkeys, generated
from the live mapping table (so it never drifts from Controller Mapping):

```
+-- Controls ----------------------------------+
|  Menu            Esc                          |
|  Quick Save/Load F5 / F9                      |
|  Fullscreen      F11                          |
|  VRAM View       F1                           |
|  Analog toggle   Tab                          |
|                                              |
|  D-Pad           Arrow keys                   |
|  Cross/Circle    Z / X                        |
|  Square/Triangle A / S                        |
|  ... (from the mapping table)                 |
|                                [ Close ]      |
+----------------------------------------------+
```

## Input and navigation

- **Focus** moves with D-pad/arrows and Tab; a visible focus ring marks it.
- **Activate** is Cross or Enter or left mouse; **cancel/back** is Circle or Esc.
- **Adjust** (sliders, spinners, dropdowns) is Left/Right or mouse wheel.
- **Menus** cycle with L1/R1; a dialog's tabs cycle with L1/R1 too.
- Mouse is optional everywhere: hover focuses, click activates, wheel scrolls.
- The overlay consumes its input; none of it reaches the frozen pad.

## Config persistence

A single human-readable config file (`jonboy.ini`, key=value, small hand-written
parser, no dependency) in the platform config dir (`$XDG_CONFIG_HOME/jonboy/` or
alongside the binary). It holds:

- window: scale, fullscreen, aspect, filter, scanlines, pause-on-focus-loss
- audio: volume, mute, latency
- core: gpu_thread, fasttick, region, bios path, fast boot
- input: the keyboard and gamepad maps for both ports, analog default
- recent: MRU game list (bounded)

Settings and mapping dialogs read it on open and write it on Apply/OK. Command-line
flags still win over config for that run, so scripted and headless use is
unchanged.

## Core plumbing: existing vs new

| Feature                 | Status | Hook |
|-------------------------|--------|------|
| Suspend/resume core     | new    | `app.paused` flag the emu loop checks |
| Single present point    | new    | refactor `present_frame` to not present |
| Open/swap disc          | exists | `cdrom_mount` / `cdrom_eject` |
| Reset / boot a game     | exists | the cold-boot/reset path, triggered live |
| Memory card insert/eject| exists | `mc_mount_slot(slot, path)` / clear |
| New memory card         | mostly | create+format a 128 KB image, then mount |
| Remappable controls     | new    | replace `pad_bit_for_key` switch with a table |
| Config file             | new    | small ini reader/writer |
| Native file dialog      | exists | `SDL_ShowOpenFileDialog` (SDL3) |
| Save / Load State       | new    | a serialization layer (the big one, below) |
| Version string          | new    | `version.h` from `git describe` in the Makefile |

### Save-state serialization (the one large dependency)

The menu's State items are inert until a snapshot layer exists. It must serialize,
behind a magic + version header keyed by title:

- CPU: GPRs, PC/next_pc, HI/LO, load-delay slot, COP0 (SR, CAUSE, EPC, ...)
- GTE: all control and data registers
- RAM (2 MB), scratchpad (1 KB), BIOS is not saved (reloaded by path)
- VRAM (1 MB) and the GPU register/draw state
- SPU: sound RAM (512 KB), voice and reverb registers, the output ring position
- DMA channel registers, the interrupt controller (I_STAT/I_MASK), the three timers
- CD-ROM: the drive position, mode, and FIFO/transfer state (not the disc image)
- pad/mc state, and the scheduler's counters and pending events

Restoring is a reverse load into the same globals, with the GPU worker drained and
the core paused (which the overlay already guarantees). Determinism is not required
across a save/load, only correctness of the resumed state.

## Phasing

A sensible build order, each shippable:

1. Harness: pause flag, single present, dim layer, and a stub menu bar that only
   has File (Open Disc, Reset, Exit) and Help (About). Proves the suspend/resume
   and composite path end to end.
2. File picker (native dialog first), Recent, Cards, and the View toggles. Now it
   is a usable front end and in-game disc/card manager with no new core subsystems.
   Done. Added File > Recent (in-memory most-recently-booted discs, becomes
   persistent in phase 3 with the config file), the View menu (Fullscreen, VRAM
   View, Aspect 4:3/Stretch/Integer, Scale presets, live Perf HUD toggle), and
   Cards Eject (a small mc_eject_slot in mc.c, the one core addition this phase).
   The file picker stayed the in-window browser rather than the async native dialog,
   since it is synchronous and controller-navigable; the native dialog can be an
   alternative later. The Perf HUD toggle made JONBOY_PERF a live SDL_AtomicInt the
   emu thread reads each pass instead of a one-shot getenv.
3. Settings dialog and config persistence, including Controller Mapping (replace
   the hardcoded key switch). Now the emulator is configurable without env vars.
   Done, as a Settings menu rather than a tabbed dialog (minimal needs): Controls...
   opens the mapping screen and Volume cycles the master gain. The hardcoded
   pad_bit_for_key switch became a mutable g_keymap table; the mapping screen lists
   all 16 buttons with their keys, rebinds by keycapture (select a row, press a
   key), and has a Reset to Defaults row. A jonboy.ini in $XDG_CONFIG_HOME/jonboy
   (or ~/.config/jonboy) persists aspect, scale, fullscreen, perf HUD, volume, the
   full keyboard map, and the recent list; it loads at startup (JONBOY_PERF still
   forces the HUD on for a run) and saves on every overlay close and at shutdown. So
   Recent now survives restarts too. Gamepad remapping is out of scope here (SDL's
   gamepad mapping drives the pad); the dialog rebinds the keyboard.
4. Save-state serialization and the State menu. The largest piece, deferred last so
   the rest ships without it.

## Harness implementation

The harness is phase 1: the pause flag, the single-present refactor, and the action
wiring. It is self-contained in `main.c` and touches no subsystem code. After it
lands, a menu is just "draw something over the frame and call an action function."

### AppContext additions

```c
typedef struct {
    /* ... existing fields ... */
    SDL_AtomicInt paused;     /* 1 asks the emu thread to stop stepping */
    SDL_AtomicInt paused_ack; /* 1 once the emu thread has parked at a frame edge */
    SDL_AtomicInt overlay;    /* 1 while the menu overlay is open (main-thread owned) */
} AppContext;
```

`paused` and `paused_ack` form a quiescence handshake so the main thread can mutate
core state (mount a disc, reset) only while the emu thread is provably idle at a
frame boundary, with no lock on the hot path. `overlay` is read only by the main
thread.

### The pause check in emu_thread

The emu loop already parks nothing between frames; add a check at the very top of
the outer `while (running)` loop, before the time accounting:

```c
while (SDL_GetAtomicInt(&app->running)) {
    if (SDL_GetAtomicInt(&app->paused)) {
        /* Park at a frame boundary: run no instructions, publish nothing. Ack so
         * the main thread knows core state is quiescent and safe to mutate. Reset
         * the clock so no backlog accrues while paused; resume starts fresh. */
        SDL_SetAtomicInt(&app->paused_ack, 1);
        SDL_Delay(4);
        last_time = SDL_GetPerformanceCounter();
        accumulator = 0.0;
        continue;
    }
    SDL_SetAtomicInt(&app->paused_ack, 0);

    uint64_t now = SDL_GetPerformanceCounter();
    /* ... existing body unchanged ... */
}
```

The check sits at the outer boundary, so a pause takes effect after the current
caught-up pass finishes stepping and publishing (a few milliseconds at most). That
is the point where `cpu`, `vram`, `spu`, `cdrom`, and the scheduler are all
internally consistent, which is exactly what the actions need.

Side effects of pausing, all automatic:

- Audio: the emu stops filling the SPU ring, the audio callback drains it and pads
  silence, so sound fades out on its own. Optionally call
  `SDL_SetAudioStreamGain(app->audio, 0.0f)` on open and restore on close for an
  instant mute.
- GPU worker: no new GP0 is submitted, so it parks on its condition variable.
- Presentation: the main thread keeps presenting the last published frame every
  vsync, so the menu stays responsive and can animate.

### Single-present refactor

Today `present_frame` presents in two places: an early `SDL_RenderPresent` plus
`return` in the compact path, and a trailing `SDL_RenderPresent` at the end. Split
it so it renders the game frame but never presents, and move the one present to the
main loop after the overlay is drawn.

Rename `present_frame` to `render_game_frame` and make two edits:

```c
/* compact path, was:
 *     SDL_RenderTexture(app->renderer, app->vram_texture, &src, NULL);
 *     SDL_RenderPresent(app->renderer);
 *     return;
 * becomes: render, then return without presenting. */
        SDL_RenderTexture(app->renderer, app->vram_texture, &src, NULL);
        return;
```

```c
/* end of the function, drop the trailing present:
 *     SDL_RenderPresent(app->renderer);   // removed
 * the function now leaves the game drawn into the renderer, unpresented. */
```

The main loop then owns the single present:

```c
    render_game_frame(&app);
    if (SDL_GetAtomicInt(&app.overlay)) {
        overlay_draw(&app);   /* dim layer, menu bar, active dialog */
    }
    SDL_RenderPresent(app.renderer);
    SDL_Delay(1);
```

`overlay_draw` is the only new rendering, and it draws after the game frame so it
composites on top. The dim is one translucent fill over the logical area:

```c
static void
overlay_dim(SDL_Renderer *r)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 153);   /* ~60% black */
    SDL_RenderFillRect(r, NULL);               /* NULL = whole logical target */
}
```

Because the overlay draws in the renderer's logical-presentation space (640x480,
letterboxed by `SDL_SetRenderLogicalPresentation`), menu coordinates are resolution
independent and the letterbox bars stay black outside it.

### Open and close (the quiescence handshake)

```c
static void
overlay_open(AppContext *app)
{
    SDL_SetAtomicInt(&app->overlay, 1);
    SDL_SetAtomicInt(&app->paused, 1);

    /* Wait until the emu thread has parked at a frame edge before any action
     * touches core state. Bounded spin with a yield; the emu acks within one
     * outer pass (a few ms). */
    while (!SDL_GetAtomicInt(&app->paused_ack)
           && SDL_GetAtomicInt(&app->running)) {
        SDL_Delay(1);
    }

    if (app->audio) {
        SDL_SetAudioStreamGain(app->audio, 0.0f);  /* optional instant mute */
    }
}

static void
overlay_close(AppContext *app)
{
    if (app->audio) {
        SDL_SetAudioStreamGain(app->audio, 1.0f);
    }
    SDL_SetAtomicInt(&app->paused, 0);
    SDL_SetAtomicInt(&app->overlay, 0);
}
```

Any menu action that mutates the console (disc, card, reset, boot) is called only
from the overlay, which holds `paused` with `paused_ack` observed, so the emu
thread is guaranteed parked. No lock is taken around core state: the handshake is
the mutual exclusion, and it costs nothing on the running hot path.

### Event routing

In the main event loop, intercept the overlay hotkey and, while the overlay is
open, send input to the UI instead of the pad:

```c
} else if (event.type == SDL_EVENT_KEY_DOWN) {
    if (event.key.key == SDLK_ESCAPE) {
        if (SDL_GetAtomicInt(&app.overlay)) {
            overlay_close(&app);          /* Esc backs out / closes */
        } else {
            overlay_open(&app);           /* Esc opens the menu */
        }
        continue;                          /* do not forward to the pad */
    }
    if (SDL_GetAtomicInt(&app.overlay)) {
        overlay_key(&app, event.key.key);  /* UI consumes it */
        continue;
    }
    /* ... existing F1 / Tab / pad-key handling, only when not in the overlay ... */
}
```

Note this changes Escape from "quit" to "open menu"; quit moves to File > Exit (and
the window close button still sets `running = 0`). Gamepad events are routed the
same way: while `overlay` is set, button and D-pad events drive `overlay_button`
rather than the pad. The existing `update_pad_input` call is already skipped when a
script drives input; extend that guard so it is also skipped while the overlay is
open, so the frozen game sees a neutral pad.

### Action wiring

The actions are thin: each runs on the main thread while paused, calls existing
core entry points, and returns. The ones phase 1 needs:

```c
/* Disc: hot swap through the existing CD path. Raises the lid INT5 so a running
 * game notices. Safe because the emu thread is parked. */
static void
action_open_disc(AppContext *app, const char *path)
{
    (void)app;
    cdrom_eject();
    cdrom_mount(path);
}

static void
action_close_disc(AppContext *app)
{
    (void)app;
    cdrom_eject();
}

/* Memory card: attach or swap in a slot (1 or 2 -> internal slot 0/1). */
static void
action_insert_card(AppContext *app, int slot, const char *path)
{
    (void)app;
    mc_mount_slot(slot, path);
}

/* Reset: re-run the machine init, then resume. The existing cold-boot path does
 * this; factor its core into a reset_machine() both it and this call. */
static void
action_reset(AppContext *app)
{
    reset_machine();          /* CPU/COP0/GTE reset, reload BIOS, remount CD */
    app->have_frame = 0;      /* drop the stale presented frame */
}

/* Boot a different game: reset, mount the new disc, resume. */
static void
action_boot_game(AppContext *app, const char *path)
{
    cdrom_eject();
    cdrom_mount(path);
    reset_machine();
    app->have_frame = 0;
}
```

`reset_machine()` is the one small extraction the harness asks of the core: lift the
reset-and-load sequence the windowed startup already runs (BIOS map, `psx_gpu_reset`
and friends, CD remount) into a function the menu can call live. Everything else
(`cdrom_mount`, `cdrom_eject`, `mc_mount_slot`) exists and is called as is.

With this in place, a menu item is just: capture a path (native dialog or the
in-window browser), call the matching `action_*`, and on a boot/reset leave the
overlay open or close it to resume. The draw side (`overlay_draw`, `overlay_key`,
`overlay_button`) is the widget kit from the spec above, built on top of this
harness.

### What phase 1 delivers

A working suspend/resume overlay with File (Boot Disc, Swap Disc, Close Disc, Reset,
Exit), Cards (Insert Card 1/2), and Help (About), driven by keyboard and gamepad
over the dimmed game. No save-state, no config file, no settings: those are later
phases.

### Status: implemented

Phase 1 is in `main.c`. Notes on what shipped versus this plan:

- `reset_machine()` lives in `main.c` (RAM clear, `psx_hardware_reset`,
  `cpu_reset(0xBFC00000)`), the same sequence the loader reboot already used. No
  change was needed outside `main.c`.
- The whole overlay (state, file browser, draw, actions) is in `main.c`, not a
  separate module, to avoid exposing `AppContext` and the renderer in a header.
  Text is drawn with `SDL_RenderDebugText` (SDL3 built-in font, no assets); the
  file browser uses `opendir`/`readdir`/`stat`.
- Open keys: `Esc` toggles the menu (so `Esc` no longer quits; Exit and the window
  close button quit), and the gamepad Guide button opens it (it no longer toggles
  analog on the pad; keyboard `Tab` still does). In the menu, D-pad/arrows move,
  Cross/`Z`/Enter activate, Circle/`X`/`Esc` back out.
- Cards are Insert only for now (mount or create via `mc_mount_slot`); Eject needs a
  small `mc_eject_slot` in `mc.c`, deferred with the rest of phase 2/3.
- The file picker is the in-window browser (filtered to disc extensions for
  Boot/Swap), not the native `SDL_ShowOpenFileDialog`, so it is synchronous and
  controller-navigable. The native dialog can be offered later as an alternative.

Validated live: the menu, file picker, and About render over a dimmed frozen frame,
and Boot Disc end to end mounts a disc, resets, and resumes into the booting game
(verified booting Final Fantasy Tactics from the BIOS). Conformance stays 29/29 and
the deterministic modes are untouched (they never create the window or the overlay).
