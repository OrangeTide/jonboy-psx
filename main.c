#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "cpu.h"
#include "gpu.h"
#include "inject.h"
#include "mem.h"
#include "pad.h"
#include "psio.h"
#include "vram.h"
#include "rom.h"
#include "cdrom.h"
#include "spu.h"
#include "mc.h"
#include "mdec.h"
#include "sched.h"
#include "script.h"
#include "gdb.h"
#include "savestate.h"

// The digital pad buttons in a fixed display order, with the key bound to each.
// The keyboard map is mutable (Controller Mapping dialog, persisted in the config)
// so g_keymap[i] is the key for ov_pad_buttons[i].bit.
#define PAD_BTN_COUNT 16
static const struct { uint16_t bit; const char *name; } ov_pad_buttons[PAD_BTN_COUNT] = {
    { PAD_BTN_UP, "Up" },       { PAD_BTN_DOWN, "Down" },
    { PAD_BTN_LEFT, "Left" },   { PAD_BTN_RIGHT, "Right" },
    { PAD_BTN_CROSS, "Cross" }, { PAD_BTN_CIRCLE, "Circle" },
    { PAD_BTN_SQUARE, "Square" }, { PAD_BTN_TRIANGLE, "Triangle" },
    { PAD_BTN_L1, "L1" },       { PAD_BTN_R1, "R1" },
    { PAD_BTN_L2, "L2" },       { PAD_BTN_R2, "R2" },
    { PAD_BTN_L3, "L3" },       { PAD_BTN_R3, "R3" },
    { PAD_BTN_START, "Start" }, { PAD_BTN_SELECT, "Select" },
};

static SDL_Keycode g_keymap[PAD_BTN_COUNT] = {
    SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT,
    SDLK_Z, SDLK_X, SDLK_A, SDLK_S,
    SDLK_Q, SDLK_W, SDLK_E, SDLK_R,
    SDLK_C, SDLK_V, SDLK_RETURN, SDLK_RSHIFT,
};

static void
keymap_reset_defaults(void)
{
    static const SDL_Keycode defaults[PAD_BTN_COUNT] = {
        SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT,
        SDLK_Z, SDLK_X, SDLK_A, SDLK_S,
        SDLK_Q, SDLK_W, SDLK_E, SDLK_R,
        SDLK_C, SDLK_V, SDLK_RETURN, SDLK_RSHIFT,
    };
    memcpy(g_keymap, defaults, sizeof g_keymap);
}

// Map a host key to a digital pad button bit, or 0 if it is not a pad key.
static uint16_t
pad_bit_for_key(SDL_Keycode key)
{
    for (int i = 0; i < PAD_BTN_COUNT; i++) {
        if (g_keymap[i] == key) {
            return ov_pad_buttons[i].bit;
        }
    }
    return 0;
}

// Map an SDL analog axis (-32768..32767) to the pad's 0..255 range, 0x80 center.
// The sign conventions line up: SDL right/down are positive, which the pad also
// reports as the high end.
static uint8_t
axis_to_byte(int16_t v)
{
    return (uint8_t)(((int)v + 32768) >> 8);
}

// Combine keyboard and (if present) gamepad input into the pad state for a frame.
// Buttons from both devices AND together (both are active-low, so either can
// press a button); the gamepad drives the analog sticks, which rest centered
// when no gamepad is connected.
static void
update_pad_input(SDL_Gamepad *gp, uint16_t kbd_buttons)
{
    uint16_t gpb = 0xFFFF;

    if (gp) {
        static const struct {
            SDL_GamepadButton button;
            uint16_t bit;
        } map[] = {
            { SDL_GAMEPAD_BUTTON_DPAD_UP,        PAD_BTN_UP },
            { SDL_GAMEPAD_BUTTON_DPAD_DOWN,      PAD_BTN_DOWN },
            { SDL_GAMEPAD_BUTTON_DPAD_LEFT,      PAD_BTN_LEFT },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     PAD_BTN_RIGHT },
            { SDL_GAMEPAD_BUTTON_SOUTH,          PAD_BTN_CROSS },
            { SDL_GAMEPAD_BUTTON_EAST,           PAD_BTN_CIRCLE },
            { SDL_GAMEPAD_BUTTON_WEST,           PAD_BTN_SQUARE },
            { SDL_GAMEPAD_BUTTON_NORTH,          PAD_BTN_TRIANGLE },
            { SDL_GAMEPAD_BUTTON_START,          PAD_BTN_START },
            { SDL_GAMEPAD_BUTTON_BACK,           PAD_BTN_SELECT },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  PAD_BTN_L1 },
            { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_BTN_R1 },
            { SDL_GAMEPAD_BUTTON_LEFT_STICK,     PAD_BTN_L3 },
            { SDL_GAMEPAD_BUTTON_RIGHT_STICK,    PAD_BTN_R3 },
        };

        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
            if (SDL_GetGamepadButton(gp, map[i].button)) {
                gpb &= ~map[i].bit;
            }
        }

        // The analog triggers stand in for L2/R2 past a quarter press.
        if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8192) {
            gpb &= (uint16_t)~PAD_BTN_L2;
        }
        if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8192) {
            gpb &= (uint16_t)~PAD_BTN_R2;
        }

        pad_lstick_x = axis_to_byte(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX));
        pad_lstick_y = axis_to_byte(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));
        pad_rstick_x = axis_to_byte(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX));
        pad_rstick_y = axis_to_byte(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));
    } else {
        pad_lstick_x = pad_lstick_y = 0x80;
        pad_rstick_x = pad_rstick_y = 0x80;
    }

    pad_buttons = kbd_buttons & gpb;
}

// Boot options shared with the SDL run function (which SDL_RunApp calls with a
// fixed signature, so they cannot be passed as arguments).
static int g_psio_boot = 0;               // inject the PSIO boot stub over the BIOS
static const char *g_psio_sd = NULL;       // optional SD-card image to mount
static const char *g_psio_firmware = NULL; // optional PSIO firmware ROM image
static const char *g_bios_path = "bios.bin"; // BIOS image to load
static const char *g_sideload_exe = NULL;    // PS-EXE to run atop the booted kernel
static const char *g_games_dir = NULL;       // directory the loader menu browses
static const char *g_cd_path = NULL;         // disc image to mount (.bin/.cue)
static const char *g_memcard_path = NULL;    // slot-1 memory-card image (raw 128 KB)
static const char *g_memcard2_path = NULL;   // slot-2 memory-card image (raw 128 KB)
static const char *g_script_path = NULL;     // timed system-event script file

// The disc currently mounted (startup --cd or a menu boot/swap). Save-state slots
// are keyed by this so one game's slots do not collide with another's.
static char g_current_disc[PATH_MAX];

#define TARGET_FPS 60

// The CPU runs at ~33.87 MHz, which is 564480 cycles per 60 Hz frame. Each
// step_cpu call advances two cycles, so a frame is half that many steps.
#define STEPS_PER_FRAME (564480 / 2)

// Emulated wall-clock seconds represented by a step count: at the target frame
// rate, STEPS_PER_FRAME steps is one 60th of a second of console time.
static double
emulated_seconds(uint64_t steps)
{
    return (double)steps / (double)STEPS_PER_FRAME / (double)TARGET_FPS;
}

// The interpreter-versus-real-time metric for the unpaced run modes (headless,
// exe, boot). These loops run as fast as the host allows, so comparing the
// console time they cover against the wall time they took is a direct real-time
// ratio: 1.0x is exactly real-time, above is headroom, below cannot keep up.
// This is the number to track across optimizations and across hosts (desktop
// versus Pi 3/4/5).
static void
report_realtime(uint64_t steps, uint64_t start_counter)
{
    double freq = (double)SDL_GetPerformanceFrequency();
    double wall = (double)(SDL_GetPerformanceCounter() - start_counter) / freq;
    double emu = emulated_seconds(steps);
    double ratio = wall > 0.0 ? emu / wall : 0.0;
    double mips = wall > 0.0 ? (double)steps / wall / 1e6 : 0.0;

    printf("Perf: %.2fs console time in %.2fs wall = %.2fx real-time "
           "(%.1fM steps/s)\n", emu, wall, ratio, mips);
}

int SDLCALL jonboy_runapp(int argc, char *argv[]);

// Shell entry point: the BIOS reaches it once the kernel is fully initialized.
#define KERNEL_SHELL_ENTRY 0x80030000

// Default loader menu program, auto-sideloaded when --games is set without an
// explicit --sideload. Relative to the working directory, matching the build.
#define LOADER_DEFAULT_PATH "_out/loader.exe"

// Service the sideload-at-kernel-up and loader menu boot requests. Called once
// per CPU step (before step_cpu) from both the headless and windowed run loops,
// so the loader behaves the same in each. *kernel_up and *sideloaded track boot
// progress across calls.
static void
service_loader_boot(int *kernel_up, int *sideloaded)
{
    // Fast path out of the per-instruction cost: the body only ever acts when
    // the CPU reaches the shell entry (to note kernel-up and run a one-time
    // sideload) or when the guest menu has strobed a boot request. Both are
    // rare, so skip the rest on the common step where neither holds.
    if (cpu.PC != KERNEL_SHELL_ENTRY && !loader_boot_pending()) {
        return;
    }

    if (cpu.PC == KERNEL_SHELL_ENTRY) {
        *kernel_up = 1;
    }

    // Sideload a PS-EXE over the booted kernel once it reaches the shell entry.
    if (g_sideload_exe && !*sideloaded && cpu.PC == KERNEL_SHELL_ENTRY) {
        uint32_t pc, gp, sp;

        if (mem_load_exe(g_sideload_exe, &pc, &gp, &sp) != 0) {
            fprintf(stderr, "sideload: failed to load %s\n", g_sideload_exe);
            g_sideload_exe = NULL; // do not retry every step
            return;
        }

        cpu.PC = pc;
        cpu.next_pc = pc + 4;
        cpu.regs[28] = gp;
        cpu.regs[29] = sp;
        cpu.regs[30] = sp;
        *sideloaded = 1;
        printf("Sideloaded %s at PC=0x%08X\n", g_sideload_exe, pc);
        return;
    }

    // The guest menu picked an image and strobed the boot command.
    if (loader_boot_pending() && *kernel_up) {
        const char *path = loader_boot_path();

        if (loader_boot_is_cd()) {
            // A CD image boots the way a real disc does: mount it, then reset the
            // machine so the BIOS runs its disc boot sequence. Stop re-sideloading
            // the menu, and clear RAM and hardware so the reboot starts clean.
            if (path && cdrom_mount(path) == 0) {
                printf("Loader mounting disc %s, rebooting\n", path);
                g_sideload_exe = NULL;
                *sideloaded = 0;
                *kernel_up = 0;
                memset(system_ram, 0, RAM_SIZE);
                psx_hardware_reset();
                cpu_reset(0xBFC00000);
            } else {
                fprintf(stderr, "loader: failed to mount %s\n",
                        path ? path : "(none)");
            }
        } else {
            // A PS-EXE loads over RAM and jumps to its entry.
            uint32_t pc, gp, sp;

            if (path && mem_load_exe(path, &pc, &gp, &sp) == 0) {
                cpu.PC = pc;
                cpu.next_pc = pc + 4;
                cpu.regs[28] = gp;
                cpu.regs[29] = sp;
                cpu.regs[30] = sp;
                printf("Loader booted %s at PC=0x%08X\n", path, pc);
            } else {
                fprintf(stderr, "loader: failed to boot %s\n",
                        path ? path : "(none)");
            }
        }

        loader_clear_boot();
    }
}

// Simple application state structure
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *vram_texture; // Target texture handle for rendering
    SDL_Texture *disp24_texture; // RGB24 unpack target for 24-bit display mode
    SDL_AudioStream *audio;    // SPU output sink (NULL if audio failed to open)
    SDL_Gamepad *gamepad;      // open analog gamepad, or NULL (keyboard only)
    bool full_vram_view;       // F1 toggles between display area and full VRAM

    // The deterministic emulation core runs on its own thread so a slow present
    // or a blocked event pump (a window drag) never stalls emulation or audio,
    // and vice versa. The main thread owns events, input, and presentation.
    SDL_Thread *emu;
    SDL_Thread *gpu_worker;    // GPU rasterization worker (NULL unless threaded)
    SDL_AtomicInt running;     // 0 stops the emu thread (set by the main thread)

    // Frame handoff: a lock-free triple buffer. The emu thread transposes VRAM
    // into its private buffer and publishes it; the main thread takes the latest
    // published buffer to present. Neither thread ever waits on the other, and
    // neither copies under a lock. frame_slot holds the index of the last
    // published buffer plus FRAME_FRESH when it has not been presented yet;
    // produce_idx/consume_idx are each private to one thread.
    SDL_AtomicInt frame_slot;
    int produce_idx;   // emu-thread private: the buffer it writes into
    int consume_idx;   // main-thread private: the buffer it presents from
    int have_frame;    // main-thread: a frame has been received at least once

    // Native menu overlay (docs/menu-overlay.md). paused asks the emu thread to
    // stop stepping; paused_ack confirms it has parked at a frame boundary, so the
    // main thread can mutate core state (mount a disc, reset) without a lock. overlay
    // is 1 while the menu is open and is read only by the main thread.
    SDL_AtomicInt paused;
    SDL_AtomicInt paused_ack;
    SDL_AtomicInt overlay;

    // View and audio settings the menu changes (main-thread owned, persisted).
    int filter_idx;    // 0 = linear, 1 = nearest (texture scale filter)
    int scale_idx;     // index into the window-size presets
    int fullscreen;    // 0/1
    int volume;        // master volume, 0..100
} AppContext;

// Three linear row-major BGR555 snapshots of VRAM, one per triple-buffer slot,
// with the display geometry that goes with each.
#define FRAME_BUFS 3
#define FRAME_IDX_MASK 0x3
#define FRAME_FRESH 0x4
static uint16_t g_frame_buf[FRAME_BUFS][VRAM_HEIGHT * VRAM_WIDTH];
static struct {
    int dx, dy, dw, dh, blank, bpp24;
    // full = 1 when g_frame_buf holds a whole-VRAM column-major snapshot; 0 when
    // it holds only the compact dw x dh display region. The producer sets it per
    // frame so the consumer always transposes the layout that was actually
    // published, even across an F1 toggle.
    int full;
} g_frame_meta[FRAME_BUFS];

// Set by the main thread (F1) and read by the emu thread in publish_frame: when
// the debug full-VRAM view is on, the producer must publish the whole snapshot,
// not just the display region.
static SDL_AtomicInt g_full_vram;

// Live performance HUD toggle. Initialized from JONBOY_PERF at startup; the View
// menu flips it at runtime, and the emu thread reads it each pass.
static SDL_AtomicInt g_perf_hud;

// SPU output is 44.1 kHz stereo 16-bit. The audio device pulls on demand through
// this callback, which runs on SDL's own audio thread and drains the SPU ring.
// So audio is produced by the emu thread and consumed here, decoupled from the
// main thread entirely: a blocked event pump or slow present cannot starve it.
#define AUDIO_RATE 44100

// Audio-paced timing cushion. The audio device drains the SPU ring at exactly
// 44.1 kHz, so keeping the ring filled to a target depth makes the core run at
// real time with a jitter buffer: brief scheduling stalls are covered by the
// buffered audio instead of starving the device. The depth trades latency for
// jitter tolerance; with the emu thread raised to HIGH priority (and the audio
// callback on the real-time scheduler) jitter is small, so a shorter cushion
// holds. The default is chosen by whether the emu thread's priority bump was
// granted: a small cushion when it was (measured to hold with headroom at 20 ms,
// so 40 ms leaves 2x margin), a larger jitter buffer when the thread is left at
// normal scheduling. JONBOY_AUDIO_CUSHION_MS overrides either, clamped to leave the
// 186 ms ring room. MAX_CATCHUP_FRAMES bounds how many frames one pass may run while
// refilling the cushion, so a long stall cannot make a single pass run away.
#define AUDIO_CUSHION_MS_PRIORITY 40
#define AUDIO_CUSHION_MS_NORMAL   80
#define AUDIO_CUSHION_MS_MAX 150
#define MAX_CATCHUP_FRAMES 8

// Target ring depth in frames, resolved at emu-thread start (see AUDIO_CUSHION_MS_*).
static int g_audio_cushion_frames = AUDIO_RATE * AUDIO_CUSHION_MS_NORMAL / 1000;

// Audio-dropout metric: the audio device pulls a fixed number of frames each
// callback. When the SPU ring cannot supply them all, the device gets silence
// and the player hears a gap. Each such short callback counts one underrun; the
// perf HUD in the emu thread reports and clears the count once a second.
static SDL_AtomicInt g_audio_underruns;

static void SDLCALL
audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    (void)userdata;
    (void)total;

    // Feed only what the device asked for. The SPU ring bounds how far ahead
    // emulation can run; if it is empty (an underrun) we simply hand over less,
    // and SDL fills the gap with silence.
    int requested = additional / (int)(2 * sizeof(int16_t));
    int frames = requested;
    int16_t buf[1024 * 2];

    while (frames > 0) {
        int want = frames > 1024 ? 1024 : frames;
        int n = spu_output_pull(buf, want);
        if (n <= 0) {
            break;
        }
        SDL_PutAudioStreamData(stream, buf, n * 2 * (int)sizeof(int16_t));
        frames -= n;
    }

    // The ring ran dry before the device's request was met: one dropout event.
    if (frames > 0) {
        SDL_AddAtomicInt(&g_audio_underruns, 1);
    }
}

// Emu thread: transpose the column-major VRAM into its private snapshot buffer
// and publish it. The exchange hands the just-written buffer to the main thread
// (as the fresh slot) and takes back whichever buffer the slot held, all without
// a lock and without waiting on the main thread.

// Compute the display region the producer publishes this frame: origin (dx,dy)
// and size (dw,dh) clamped to VRAM, the blank and 24-bit flags, and whether a
// full-VRAM snapshot is needed (debug view, 24-bit, blank, or an empty region)
// instead of just the compact region. Shared by publish_frame and the benchmark
// so their copy sizes match exactly.
static void
frame_display_region(int *dx_o, int *dy_o, int *dw_o, int *dh_o,
                     int *blank_o, int *bpp24_o, int *full_o)
{
    // Present only the columns and scanlines the display range covers, so a mode
    // that draws a smaller picture than it allows (Chrono Trigger's 224-line,
    // sub-320-wide screens) does not leak the stale VRAM around the picture.
    int dx = gpu.disp_x;
    int dy = gpu.disp_y;
    int dw = psx_gpu_active_width();
    int dh = psx_gpu_active_height();
    int blank = gpu.disp_blank;
    int bpp24 = gpu.disp_24bpp;

    // Clamp to the canvas so the compact copy never reads past the end of VRAM.
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    if (dx > VRAM_WIDTH) dx = VRAM_WIDTH;
    if (dy > VRAM_HEIGHT) dy = VRAM_HEIGHT;
    if (dx + dw > VRAM_WIDTH) dw = VRAM_WIDTH - dx;
    if (dy + dh > VRAM_HEIGHT) dh = VRAM_HEIGHT - dy;
    if (dw < 0) dw = 0;
    if (dh < 0) dh = 0;

    *dx_o = dx;
    *dy_o = dy;
    *dw_o = dw;
    *dh_o = dh;
    *blank_o = blank;
    *bpp24_o = bpp24;
    // Normal 15-bit gameplay needs only the display rectangle. The full snapshot
    // is still published for the debug view (F1), for 24-bit display (whose
    // unpack reads a wider, packed span), and when blank, where the compact
    // region would not be enough.
    *full_o = SDL_GetAtomicInt(&g_full_vram) || bpp24 || blank || dw == 0 || dh == 0;
}

// Copy the current VRAM into dst in the layout frame_display_region selected: the
// whole column-major snapshot when full, else the compact dw x dh region (~150 KB
// for a normal display instead of the whole 1 MB). vram.canvas is column-major,
// so each display column is dh contiguous words.
static void
publish_vram_region(uint16_t *dst, int dx, int dy, int dw, int dh, int full)
{
    if (full) {
        memcpy(dst, vram.canvas,
               (size_t)VRAM_WIDTH * VRAM_HEIGHT * sizeof(uint16_t));
    } else {
        for (int x = 0; x < dw; x++) {
            memcpy(&dst[(size_t)x * dh], &vram.canvas[dx + x][dy],
                   (size_t)dh * sizeof(uint16_t));
        }
    }
}

// Transpose a published column-major snapshot to row-major. The full form covers
// the whole VRAM (snap[x*H+y] -> dst[y*W+x]); the region form covers a compact
// dw x dh region (snap[x*dh+y] -> dst[y*dw+x]).
static void
transpose_full_rowmajor(uint16_t *dst, const uint16_t *snap)
{
    for (int y = 0; y < VRAM_HEIGHT; y++) {
        uint16_t *row = &dst[(size_t)y * VRAM_WIDTH];
        for (int x = 0; x < VRAM_WIDTH; x++) {
            row[x] = snap[(size_t)x * VRAM_HEIGHT + y];
        }
    }
}

static void
transpose_region_rowmajor(uint16_t *dst, const uint16_t *snap, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        uint16_t *row = &dst[(size_t)y * dw];
        for (int x = 0; x < dw; x++) {
            row[x] = snap[(size_t)x * dh + y];
        }
    }
}

// GPU worker thread: drain the GP0 command ring and rasterize into VRAM, in
// parallel with the emulation thread. Runs only when gpu_threaded is set. It
// keeps going until gpu_stop, which the main thread sets after the emu thread has
// joined, then drains any remaining commands so the final frame is complete.
static int SDLCALL
gpu_worker_thread(void *data)
{
    AppContext *app = data;

    (void)app;
    while (!gpu_worker_should_stop()) {
        if (!gpu_worker_pop_and_run()) {
            gpu_worker_wait(); // ring empty: park until a command or stop, not a spin
        }
    }
    while (gpu_worker_pop_and_run()) {
        /* drain whatever is left before exiting */
    }
    return 0;
}

static void
publish_frame(AppContext *app)
{
    // Present fence: the worker must finish every draw queued this frame before we
    // snapshot VRAM. A no-op when synchronous.
    gpu_sync();

    int dx, dy, dw, dh, blank, bpp24, full;
    frame_display_region(&dx, &dy, &dw, &dh, &blank, &bpp24, &full);
    publish_vram_region(g_frame_buf[app->produce_idx], dx, dy, dw, dh, full);

    g_frame_meta[app->produce_idx].dx = dx;
    g_frame_meta[app->produce_idx].dy = dy;
    g_frame_meta[app->produce_idx].dw = dw;
    g_frame_meta[app->produce_idx].dh = dh;
    g_frame_meta[app->produce_idx].blank = blank;
    g_frame_meta[app->produce_idx].bpp24 = bpp24;
    g_frame_meta[app->produce_idx].full = full;

    // SDL_SetAtomicInt is an atomic exchange with a full memory barrier, so the
    // buffer writes above are visible to the main thread once it reads the fresh
    // slot. It returns the previous slot value; that buffer is now ours to reuse.
    int old = SDL_SetAtomicInt(&app->frame_slot, app->produce_idx | FRAME_FRESH);
    app->produce_idx = old & FRAME_IDX_MASK;
}

// Main thread: take the latest published buffer (if any) and render it into the
// renderer. When no new frame has been published since last time, it re-renders the
// buffer it already owns, so the display keeps showing the last frame. This does not
// present: the main loop presents once, after the menu overlay (if open) is drawn on
// top, so there is a single present point per iteration.
static void
render_game_frame(AppContext *app)
{
    if (SDL_GetAtomicInt(&app->frame_slot) & FRAME_FRESH) {
        int old = SDL_SetAtomicInt(&app->frame_slot, app->consume_idx);
        app->consume_idx = old & FRAME_IDX_MASK;
        app->have_frame = 1;
    }

    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    SDL_RenderClear(app->renderer);

    if (app->have_frame) {
        const uint16_t *snap = g_frame_buf[app->consume_idx];
        int dx = g_frame_meta[app->consume_idx].dx;
        int dy = g_frame_meta[app->consume_idx].dy;
        int dw = g_frame_meta[app->consume_idx].dw;
        int dh = g_frame_meta[app->consume_idx].dh;

        if (!g_frame_meta[app->consume_idx].full) {
            // Fast path: snap holds only the compact column-major dw x dh display
            // region (snap[x*dh + y]). Transpose just that rectangle to row-major
            // and upload it to the top-left of the texture, then render it. This
            // is ~0.15 ms instead of the ~1.24 ms a whole-VRAM transpose costs.
            static uint16_t region[VRAM_HEIGHT * VRAM_WIDTH];
            transpose_region_rowmajor(region, snap, dw, dh);

            void *pixels;
            int pitch;
            SDL_Rect area = { 0, 0, dw, dh };
            if (dw > 0 && dh > 0
                && SDL_LockTexture(app->vram_texture, &area, &pixels, &pitch)) {
                uint8_t *dst = pixels;
                for (int y = 0; y < dh; y++) {
                    memcpy(dst + (size_t)y * pitch,
                           &region[(size_t)y * dw],
                           (size_t)dw * sizeof(uint16_t));
                }
                SDL_UnlockTexture(app->vram_texture);
            }

            SDL_FRect src = { 0.0f, 0.0f, (float)dw, (float)dh };
            SDL_RenderTexture(app->renderer, app->vram_texture, &src, NULL);
            return;
        }

        // Full-snapshot path (debug view, 24-bit display, or blank): the buffer
        // is a whole-VRAM column-major snapshot (snap[x*H + y]). Transpose all of
        // it to row-major, which the display paths below read.
        static uint16_t rowmajor[VRAM_HEIGHT * VRAM_WIDTH];
        transpose_full_rowmajor(rowmajor, snap);

        void *pixels;
        int pitch;
        if (SDL_LockTexture(app->vram_texture, NULL, &pixels, &pitch)) {
            uint8_t *dst = pixels;
            const uint16_t *src = rowmajor;
            for (int y = 0; y < VRAM_HEIGHT; y++) {
                memcpy(dst + (size_t)y * pitch,
                       &src[(size_t)y * VRAM_WIDTH],
                       VRAM_WIDTH * sizeof(uint16_t));
            }
            SDL_UnlockTexture(app->vram_texture);
        }

        if (app->full_vram_view) {
            // Debug view: the entire 1024x512 VRAM stretched to the window.
            SDL_RenderTexture(app->renderer, app->vram_texture, NULL, NULL);
        } else if (g_frame_meta[app->consume_idx].blank) {
            // A disabled display leaves the window cleared (black).
        } else if (g_frame_meta[app->consume_idx].bpp24) {
            // 24-bit display mode: unpack the display area's packed 24-bit RGB
            // (three bytes per pixel, spanning the 16-bit VRAM words) into the
            // RGB24 texture, then present it. The horizontal origin dx is in
            // VRAM columns; each output pixel reads three consecutive bytes.
            void *pixels;
            int pitch;
            if (dw > VRAM_WIDTH) dw = VRAM_WIDTH;
            if (dh > VRAM_HEIGHT) dh = VRAM_HEIGHT;
            SDL_Rect area = { 0, 0, dw, dh };
            if (SDL_LockTexture(app->disp24_texture, &area, &pixels, &pitch)) {
                const uint16_t *src = rowmajor;
                for (int y = 0; y < dh; y++) {
                    const uint16_t *row = &src[(size_t)((dy + y) & 0x1FF) * VRAM_WIDTH];
                    uint8_t *out = (uint8_t *)pixels + (size_t)y * pitch;
                    for (int x = 0; x < dw; x++) {
                        int b0 = dx * 2 + x * 3;
                        uint16_t w0 = row[(b0 >> 1) & 0x3FF];
                        uint16_t w1 = row[((b0 + 1) >> 1) & 0x3FF];
                        uint16_t w2 = row[((b0 + 2) >> 1) & 0x3FF];
                        out[x * 3 + 0] = (b0 & 1) ? (w0 >> 8) : (w0 & 0xFF);
                        out[x * 3 + 1] = ((b0 + 1) & 1) ? (w1 >> 8) : (w1 & 0xFF);
                        out[x * 3 + 2] = ((b0 + 2) & 1) ? (w2 >> 8) : (w2 & 0xFF);
                    }
                }
                SDL_UnlockTexture(app->disp24_texture);
            }
            SDL_FRect src = { 0.0f, 0.0f, (float)dw, (float)dh };
            SDL_RenderTexture(app->renderer, app->disp24_texture, &src, NULL);
        } else {
            // Normal view: only the GPU's display area, as a TV would.
            SDL_FRect src = { (float)dx, (float)dy, (float)dw, (float)dh };
            SDL_RenderTexture(app->renderer, app->vram_texture, &src, NULL);
        }
    }
}

// The emulation thread: run the deterministic core (CPU, GPU, SPU, DMA, CD) paced
// to real time, push audio, and publish each finished frame. It reads pad input
// from the globals the main thread updates. Nothing here touches SDL video.
static int SDLCALL
emu_thread(void *data)
{
    AppContext *app = data;
    int kernel_up = 0;
    int sideloaded = 0;

    // Run the audio producer a notch above normal so the scheduler is less likely
    // to leave the SPU ring unfilled under load, which lets the pacing cushion stay
    // small. HIGH (not forced real-time) is deliberate: this is a long-running
    // compute loop, and a forced real-time compute thread risks rtkit's runtime
    // budget (SIGKILL) and can starve the compositor. The call fails harmlessly to
    // normal scheduling where the priority is not permitted.
    bool elevated = SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);

    // Resolve the audio cushion depth. With the priority bump granted, jitter is
    // small and a short cushion holds; without it, use a larger buffer.
    // JONBOY_AUDIO_CUSHION_MS overrides either, clamped to a sane range (0 disables
    // the run-ahead and leaves pure real-time pacing, the old near-empty-ring case).
    {
        int ms = elevated ? AUDIO_CUSHION_MS_PRIORITY : AUDIO_CUSHION_MS_NORMAL;
        const char *cm = SDL_getenv("JONBOY_AUDIO_CUSHION_MS");
        if (cm) {
            ms = SDL_atoi(cm);
        }
        if (ms < 0) {
            ms = 0;
        }
        if (ms > AUDIO_CUSHION_MS_MAX) {
            ms = AUDIO_CUSHION_MS_MAX;
        }
        g_audio_cushion_frames = AUDIO_RATE * ms / 1000;
    }

    // The loader/sideload service only ever acts when a sideload EXE or a loader
    // games directory is configured. For a plain BIOS or disc boot neither is,
    // so skip the per-step call entirely and pay only a predicted branch. This
    // keeps two function calls per instruction off the emulation thread.
    int service_active = (g_sideload_exe != NULL) || (g_games_dir != NULL);

    uint64_t last_time = SDL_GetPerformanceCounter();
    double accumulator = 0.0;
    double time_bridge = 1.0 / TARGET_FPS;
    double counts_per_sec = (double)SDL_GetPerformanceFrequency();

    // Pacing has two terms. The wall-clock accumulator is the real-time floor: it
    // runs a frame every time_bridge of elapsed wall time, so the core always keeps
    // real time even if the audio device is absent or stalls. On top of that, when
    // an audio device is open, the core runs ahead to keep the SPU ring filled to
    // AUDIO_CUSHION_FRAMES. The device drains the ring at exactly 44.1 kHz, so this
    // run-ahead self-limits at the cushion and then the floor maintains it; the
    // cushion is a jitter buffer that a brief stall drains instead of starving the
    // audio callback. A deep stall rebuilds it automatically on the next passes.
    int audio_paced = (app->audio != NULL);
    int audio_started = !audio_paced; // resume the device once the cushion is first met

    // Live real-time metric (JONBOY_PERF=1). busy_acc is the wall time spent
    // actually running instructions; comparing it to the wall time that elapsed
    // gives the fraction of the real-time budget the interpreter consumes. Below
    // 100% there is headroom; at 100% with fewer than 60 emulated frames the core
    // is saturated and audio underruns follow.
    uint64_t report_last = last_time;
    double busy_acc = 0.0;
    int frames_acc = 0;

    while (SDL_GetAtomicInt(&app->running)) {
        if (SDL_GetAtomicInt(&app->paused)) {
            // Park at a frame boundary: run no instructions, publish nothing, and
            // ack so the main thread knows core state is quiescent and safe to
            // mutate (mount a disc, reset). Reset the clock so no backlog accrues
            // while paused; resuming starts fresh.
            SDL_SetAtomicInt(&app->paused_ack, 1);
            SDL_Delay(4);
            last_time = SDL_GetPerformanceCounter();
            accumulator = 0.0;
            continue;
        }
        SDL_SetAtomicInt(&app->paused_ack, 0);

        uint64_t now = SDL_GetPerformanceCounter();
        accumulator += (double)(now - last_time) / counts_per_sec;
        last_time = now;

        // Cap the backlog so a slow patch never spirals into endless catch-up.
        if (accumulator > 4.0 * time_bridge) {
            accumulator = 4.0 * time_bridge;
        }

        int ran = 0;
        uint64_t busy_start = SDL_GetPerformanceCounter();
        while (SDL_GetAtomicInt(&app->running)) {
            int need_realtime = accumulator >= time_bridge;
            int need_cushion = audio_paced
                               && spu_output_queued() < g_audio_cushion_frames;
            if (!need_realtime && !need_cushion) {
                break; // caught up to real time and the cushion is full
            }
            // A cushion-only burst is bounded per pass so one long stall cannot
            // make a single pass run away; the real-time floor is bounded by the
            // backlog cap above.
            if (!need_realtime && ran >= MAX_CATCHUP_FRAMES) {
                break;
            }

            for (uint32_t step = 0; step < STEPS_PER_FRAME; step++) {
                if (cpu.halted) {
                    break;
                }
                if (service_active) {
                    service_loader_boot(&kernel_up, &sideloaded);
                }
                step_cpu();
            }
            if (need_realtime) {
                accumulator -= time_bridge;
            }
            frames_acc++;
            ran++;
        }

        // Publish once per outer pass, showing the latest state rather than every
        // caught-up frame that would never be seen. Publishing is counted in the
        // busy time below so the real-time budget figure reflects every piece of
        // work the emulation thread does, not just instruction execution.
        if (ran) {
            publish_frame(app);
            busy_acc += (double)(SDL_GetPerformanceCounter() - busy_start) / counts_per_sec;
        }

        // Start audio playback only after the ring first holds a full cushion, so
        // the device never drains an empty ring on the opening frame.
        if (!audio_started && spu_output_queued() >= g_audio_cushion_frames) {
            SDL_ResumeAudioStreamDevice(app->audio);
            audio_started = 1;
        }

        if (SDL_GetAtomicInt(&g_perf_hud)) {
            double since = (double)(last_time - report_last) / counts_per_sec;
            if (since >= 1.0) {
                double load = busy_acc / since * 100.0;
                int underruns = SDL_GetAtomicInt(&g_audio_underruns);
                SDL_SetAtomicInt(&g_audio_underruns, 0);
                SDL_Log("[perf] %d fps emulated, %.0f%% of real-time budget, "
                        "audio underruns/s: %d", frames_acc, load, underruns);
                report_last = last_time;
                busy_acc = 0.0;
                frames_acc = 0;
            }
        }
        SDL_Delay(1);
    }
    return 0;
}

// ---- Native menu overlay (docs/menu-overlay.md) --------------------------
//
// Phase 1: a host-side menu drawn over the paused game with SDL_Renderer and the
// built-in debug font. It never runs on the emulated console, so showing it needs
// no save-state: the core globals are frozen while the main thread draws. Actions
// run only while the emu thread is parked (the paused/paused_ack handshake), so
// they mutate core state without a lock.

// Reset the console: clear RAM, reset the hardware (keeps the mounted disc and the
// loaded BIOS ROM), and jump to the BIOS entry. The windowed startup and the loader
// reboot run the same sequence.
static void
reset_machine(void)
{
    memset(system_ram, 0, RAM_SIZE);
    psx_hardware_reset();
    cpu_reset(0xBFC00000);
}

// Overlay screens.
enum { OV_BAR, OV_PICK, OV_ABOUT, OV_RECENT, OV_MAP, OV_SAVE, OV_LOAD };

#define OV_STATE_SLOTS 9

// What a file pick will do once a file is chosen.
enum { PICK_BOOT, PICK_SWAP, PICK_CARD1, PICK_CARD2 };

#define OV_MAX_ENTRIES 512
#define OV_NAME_MAX    255
#define OV_RECENT_MAX  8

static struct {
    int screen;                                   // OV_BAR / OV_PICK / OV_ABOUT / OV_RECENT
    int menu;                                     // open top menu
    int item;                                     // selected item in that menu

    int pick_for;                                 // PICK_* for the active picker
    char dir[PATH_MAX];                           // current browse directory
    char entries[OV_MAX_ENTRIES][OV_NAME_MAX + 1];
    int is_dir[OV_MAX_ENTRIES];
    int entry_count;
    int sel;                                      // selected entry (picker/recent)
    int scroll;                                   // first visible row

    char recent[OV_RECENT_MAX][PATH_MAX];         // most-recently-booted discs
    int recent_count;

    int capturing;                                // mapping dialog: awaiting a key

    char status[96];                              // transient one-line message
} ov;

// Window-size presets for View > Scale.
static const struct { int w, h; const char *name; } ov_scales[] = {
    { 640, 480, "640x480" }, { 960, 720, "960x720" },
    { 1280, 960, "1280x960" }, { 1600, 1200, "1600x1200" },
};
#define OV_SCALE_COUNT ((int)(sizeof ov_scales / sizeof ov_scales[0]))

static const char *const ov_filter_names[] = { "Linear", "Nearest" };

// The menu bar: top menus and their items. Kept as flat tables so navigation and
// drawing are trivial. The View/Settings labels are built dynamically (ov_item_label).
static const char *const ov_menu_titles[] = { "File", "State", "Cards", "View", "Settings", "Help" };
#define OV_MENU_COUNT 6

enum { MENU_FILE, MENU_STATE, MENU_CARDS, MENU_VIEW, MENU_SETTINGS, MENU_HELP };

static const char *const ov_file_items[] = {
    "Boot Disc...", "Swap Disc...", "Recent", "Close Disc", "Reset", "Exit"
};
static const char *const ov_state_items[] = {
    "Save State...", "Load State..."
};
static const char *const ov_card_items[] = {
    "Insert Card 1...", "Eject Card 1", "Insert Card 2...", "Eject Card 2"
};
static const char *const ov_view_items[] = {
    "Fullscreen", "VRAM View", "Filter", "Scale", "Perf HUD"
};
static const char *const ov_settings_items[] = {
    "Controls...", "Volume"
};
static const char *const ov_help_items[] = { "About" };

static const char *const *const ov_menu_items[] = {
    ov_file_items, ov_state_items, ov_card_items, ov_view_items,
    ov_settings_items, ov_help_items
};
static const int ov_menu_item_count[] = {
    (int)(sizeof ov_file_items / sizeof ov_file_items[0]),
    (int)(sizeof ov_state_items / sizeof ov_state_items[0]),
    (int)(sizeof ov_card_items / sizeof ov_card_items[0]),
    (int)(sizeof ov_view_items / sizeof ov_view_items[0]),
    (int)(sizeof ov_settings_items / sizeof ov_settings_items[0]),
    (int)(sizeof ov_help_items / sizeof ov_help_items[0]),
};

// Build the display label for a menu item. View items show their current value;
// everything else is the static string.
static void
ov_item_label(AppContext *app, int menu, int i, char *buf, size_t n)
{
    if (menu == MENU_VIEW) {
        switch (i) {
        case 0: SDL_snprintf(buf, n, "Fullscreen   %s", app->fullscreen ? "on" : "off"); return;
        case 1: SDL_snprintf(buf, n, "VRAM View    %s", app->full_vram_view ? "on" : "off"); return;
        case 2: SDL_snprintf(buf, n, "Filter       %s", ov_filter_names[app->filter_idx]); return;
        case 3: SDL_snprintf(buf, n, "Scale        %s", ov_scales[app->scale_idx].name); return;
        case 4: SDL_snprintf(buf, n, "Perf HUD     %s", SDL_GetAtomicInt(&g_perf_hud) ? "on" : "off"); return;
        }
    } else if (menu == MENU_SETTINGS) {
        switch (i) {
        case 0: SDL_snprintf(buf, n, "Controls..."); return;
        case 1: SDL_snprintf(buf, n, "Volume       %d%%", app->volume); return;
        }
    }
    SDL_snprintf(buf, n, "%s", ov_menu_items[menu][i]);
}

// Push a disc path onto the recent list, most recent first, de-duplicated.
static void
ov_recent_add(const char *path)
{
    int at = -1;
    for (int i = 0; i < ov.recent_count; i++) {
        if (strcmp(ov.recent[i], path) == 0) {
            at = i;
            break;
        }
    }
    int top = (at >= 0) ? at : (ov.recent_count < OV_RECENT_MAX
                                ? ov.recent_count++ : OV_RECENT_MAX - 1);
    for (int i = top; i > 0; i--) {
        SDL_strlcpy(ov.recent[i], ov.recent[i - 1], sizeof ov.recent[i]);
    }
    SDL_strlcpy(ov.recent[0], path, sizeof ov.recent[0]);
}

// Apply the configured master volume to the audio stream.
static void
ov_apply_volume(AppContext *app)
{
    if (app->audio) {
        SDL_SetAudioStreamGain(app->audio, app->volume / 100.0f);
    }
}

static void ov_config_save(AppContext *app); // persist on close

// Pause the core and wait until the emu thread has parked at a frame boundary, so
// the menu actions can touch core state safely. Audio goes quiet on its own while
// paused, as the SPU ring drains and the callback pads silence.
static void
overlay_open(AppContext *app)
{
    ov.screen = OV_BAR;
    ov.menu = 0;  // reopen on the first menu, so each open starts predictably
    ov.item = 0;
    ov.status[0] = '\0';
    ov.capturing = 0;
    SDL_SetAtomicInt(&app->overlay, 1);
    SDL_SetAtomicInt(&app->paused, 1);

    while (!SDL_GetAtomicInt(&app->paused_ack)
           && SDL_GetAtomicInt(&app->running)) {
        SDL_Delay(1);
    }
}

static void
overlay_close(AppContext *app)
{
    ov_config_save(app); // persist any settings changed while open
    SDL_SetAtomicInt(&app->paused, 0);
    SDL_SetAtomicInt(&app->overlay, 0);
}

// ---- file browser ----

static int
ov_name_is_disc(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return 0;
    }
    return SDL_strcasecmp(dot, ".cue") == 0 || SDL_strcasecmp(dot, ".bin") == 0
           || SDL_strcasecmp(dot, ".iso") == 0 || SDL_strcasecmp(dot, ".chd") == 0
           || SDL_strcasecmp(dot, ".img") == 0 || SDL_strcasecmp(dot, ".exe") == 0;
}

// Swap two entries (name and is_dir together).
static void
ov_swap_entries(int a, int b)
{
    char t[OV_NAME_MAX + 1];
    SDL_strlcpy(t, ov.entries[a], sizeof t);
    SDL_strlcpy(ov.entries[a], ov.entries[b], sizeof ov.entries[a]);
    SDL_strlcpy(ov.entries[b], t, sizeof ov.entries[b]);
    int d = ov.is_dir[a];
    ov.is_dir[a] = ov.is_dir[b];
    ov.is_dir[b] = d;
}

// Order: directories before files, then case-insensitive by name.
static int
ov_entry_before(int a, int b)
{
    if (ov.is_dir[a] != ov.is_dir[b]) {
        return ov.is_dir[a] > ov.is_dir[b];
    }
    return SDL_strcasecmp(ov.entries[a], ov.entries[b]) < 0;
}

// Scan a directory into ov.entries: ".." first (unless at root), then
// subdirectories, then matching files, each group alphabetical.
static void
ov_scan_dir(const char *dir)
{
    SDL_strlcpy(ov.dir, dir, sizeof ov.dir);
    ov.entry_count = 0;
    ov.sel = 0;
    ov.scroll = 0;

    int want_disc = (ov.pick_for == PICK_BOOT || ov.pick_for == PICK_SWAP);

    // ".." stays pinned at index 0; sorting starts after it.
    int sort_start = 0;
    if (strcmp(dir, "/") != 0) {
        SDL_strlcpy(ov.entries[0], "..", sizeof ov.entries[0]);
        ov.is_dir[0] = 1;
        ov.entry_count = 1;
        sort_start = 1;
    }

    DIR *d = opendir(dir);
    if (!d) {
        SDL_snprintf(ov.status, sizeof ov.status, "cannot open %s", dir);
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL && ov.entry_count < OV_MAX_ENTRIES) {
        if (e->d_name[0] == '.') {
            continue; // hidden entries and . / ..
        }

        char full[PATH_MAX + OV_NAME_MAX + 2];
        SDL_snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }

        int isdir = S_ISDIR(st.st_mode);
        if (!isdir && want_disc && !ov_name_is_disc(e->d_name)) {
            continue; // filter non-disc files when picking a disc
        }

        SDL_strlcpy(ov.entries[ov.entry_count], e->d_name,
                    sizeof ov.entries[ov.entry_count]);
        ov.is_dir[ov.entry_count] = isdir;
        ov.entry_count++;
    }
    closedir(d);

    // Insertion sort (entry counts are modest): directories first, then names.
    for (int i = sort_start + 1; i < ov.entry_count; i++) {
        for (int j = i; j > sort_start && ov_entry_before(j, j - 1); j--) {
            ov_swap_entries(j, j - 1);
        }
    }
}

static void
ov_open_picker(int pick_for)
{
    ov.pick_for = pick_for;
    const char *start = g_games_dir ? g_games_dir : ".";
    char resolved[PATH_MAX];
    if (realpath(start, resolved)) {
        ov_scan_dir(resolved);
    } else {
        ov_scan_dir(".");
    }
    ov.screen = OV_PICK;
}

// Boot a disc image: swap it in, reset, record it as recent, and resume.
static int
ov_boot_disc(AppContext *app, const char *full)
{
    cdrom_eject();
    if (cdrom_mount(full) == 0) {
        reset_machine();
        app->have_frame = 0;
        ov_recent_add(full);
        SDL_strlcpy(g_current_disc, full, sizeof g_current_disc);
        return 1; // resume into the booting disc
    }
    SDL_snprintf(ov.status, sizeof ov.status, "mount failed");
    return 0;
}

// Act on the chosen file. Returns 1 if the overlay should close (resume the game).
static int
ov_pick_choose(AppContext *app)
{
    if (ov.entry_count == 0) {
        return 0;
    }

    char full[PATH_MAX + OV_NAME_MAX + 2];
    SDL_snprintf(full, sizeof full, "%s/%s", ov.dir, ov.entries[ov.sel]);

    if (ov.is_dir[ov.sel]) {
        char resolved[PATH_MAX];
        if (realpath(full, resolved)) {
            ov_scan_dir(resolved);
        }
        return 0;
    }

    switch (ov.pick_for) {
    case PICK_BOOT:
        return ov_boot_disc(app, full);
    case PICK_SWAP:
        cdrom_eject();
        cdrom_mount(full);
        SDL_strlcpy(g_current_disc, full, sizeof g_current_disc);
        return 1; // hot swap, raises the lid INT5
    case PICK_CARD1:
        mc_mount_slot(0, full);
        ov.screen = OV_BAR;
        SDL_snprintf(ov.status, sizeof ov.status, "card 1: %s", ov.entries[ov.sel]);
        return 0;
    case PICK_CARD2:
        mc_mount_slot(1, full);
        ov.screen = OV_BAR;
        SDL_snprintf(ov.status, sizeof ov.status, "card 2: %s", ov.entries[ov.sel]);
        return 0;
    }
    return 0;
}

// Boot the selected recent disc. Returns 1 to resume.
static int
ov_recent_choose(AppContext *app)
{
    if (ov.recent_count == 0) {
        return 0;
    }
    char path[PATH_MAX];
    SDL_strlcpy(path, ov.recent[ov.sel], sizeof path);
    return ov_boot_disc(app, path);
}

// ---- View settings ----

static void
ov_apply_filter(AppContext *app)
{
    // The scaling filter is a real user choice; the display aspect is not. Nearest
    // keeps hard pixel edges, linear smooths them. Both display textures scale the
    // same way so the 15-bit and 24-bit paths match.
    SDL_ScaleMode m = app->filter_idx == 1 ? SDL_SCALEMODE_NEAREST
                                           : SDL_SCALEMODE_LINEAR;
    SDL_SetTextureScaleMode(app->vram_texture, m);
    SDL_SetTextureScaleMode(app->disp24_texture, m);
    SDL_snprintf(ov.status, sizeof ov.status, "filter: %s",
                 ov_filter_names[app->filter_idx]);
}

static void
ov_apply_scale(AppContext *app)
{
    if (app->fullscreen) {
        return; // window size has no effect while fullscreen
    }
    SDL_SetWindowSize(app->window, ov_scales[app->scale_idx].w,
                      ov_scales[app->scale_idx].h);
    SDL_snprintf(ov.status, sizeof ov.status, "scale: %s",
                 ov_scales[app->scale_idx].name);
}

static void
ov_toggle_fullscreen(AppContext *app)
{
    app->fullscreen = !app->fullscreen;
    SDL_SetWindowFullscreen(app->window, app->fullscreen);
    SDL_snprintf(ov.status, sizeof ov.status, "fullscreen: %s",
                 app->fullscreen ? "on" : "off");
}

static void
ov_toggle_vram_view(AppContext *app)
{
    app->full_vram_view = !app->full_vram_view;
    SDL_SetAtomicInt(&g_full_vram, app->full_vram_view);
    SDL_snprintf(ov.status, sizeof ov.status, "VRAM view: %s",
                 app->full_vram_view ? "on" : "off");
}

static void
ov_toggle_perf_hud(void)
{
    int on = !SDL_GetAtomicInt(&g_perf_hud);
    SDL_SetAtomicInt(&g_perf_hud, on);
    SDL_snprintf(ov.status, sizeof ov.status, "perf HUD: %s", on ? "on" : "off");
}

static void
ov_cycle_volume(AppContext *app)
{
    // 100 -> 75 -> 50 -> 25 -> 0 -> 100
    app->volume = (app->volume <= 0) ? 100 : app->volume - 25;
    ov_apply_volume(app);
    SDL_snprintf(ov.status, sizeof ov.status, "volume: %d%%", app->volume);
}

// ---- config persistence (jonboy.ini) ----

static void
ov_config_dir(char *buf, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0]) {
        SDL_snprintf(buf, n, "%s/jonboy", xdg);
    } else if (home && home[0]) {
        SDL_snprintf(buf, n, "%s/.config/jonboy", home);
    } else {
        SDL_snprintf(buf, n, ".");
    }
}

static void
ov_config_save(AppContext *app)
{
    char dir[PATH_MAX], path[PATH_MAX];
    ov_config_dir(dir, sizeof dir);
    mkdir(dir, 0755); // ignore EEXIST
    SDL_snprintf(path, sizeof path, "%s/jonboy.ini", dir);

    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "filter=%d\n", app->filter_idx);
    fprintf(f, "scale=%d\n", app->scale_idx);
    fprintf(f, "fullscreen=%d\n", app->fullscreen);
    fprintf(f, "perf_hud=%d\n", SDL_GetAtomicInt(&g_perf_hud));
    fprintf(f, "volume=%d\n", app->volume);
    for (int i = 0; i < PAD_BTN_COUNT; i++) {
        fprintf(f, "key.%s=%u\n", ov_pad_buttons[i].name, (unsigned)g_keymap[i]);
    }
    for (int i = 0; i < ov.recent_count; i++) {
        fprintf(f, "recent=%s\n", ov.recent[i]);
    }
    fclose(f);
}

static void
ov_config_load(AppContext *app)
{
    char dir[PATH_MAX], path[PATH_MAX];
    ov_config_dir(dir, sizeof dir);
    SDL_snprintf(path, sizeof path, "%s/jonboy.ini", dir);

    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) {
            *nl = '\0';
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        const char *key = line, *val = eq + 1;

        if (strcmp(key, "filter") == 0) {
            app->filter_idx = atoi(val) ? 1 : 0;
        } else if (strcmp(key, "scale") == 0) {
            int s = atoi(val);
            if (s >= 0 && s < OV_SCALE_COUNT) app->scale_idx = s;
        } else if (strcmp(key, "fullscreen") == 0) {
            app->fullscreen = atoi(val) ? 1 : 0;
        } else if (strcmp(key, "perf_hud") == 0) {
            SDL_SetAtomicInt(&g_perf_hud, atoi(val) ? 1 : 0);
        } else if (strcmp(key, "volume") == 0) {
            int v = atoi(val);
            app->volume = v < 0 ? 0 : (v > 100 ? 100 : v);
        } else if (strcmp(key, "recent") == 0) {
            if (ov.recent_count < OV_RECENT_MAX && val[0]) {
                SDL_strlcpy(ov.recent[ov.recent_count], val, sizeof ov.recent[0]);
                ov.recent_count++;
            }
        } else if (strncmp(key, "key.", 4) == 0) {
            for (int i = 0; i < PAD_BTN_COUNT; i++) {
                if (strcmp(key + 4, ov_pad_buttons[i].name) == 0) {
                    g_keymap[i] = (SDL_Keycode)strtoul(val, NULL, 10);
                    break;
                }
            }
        }
    }
    fclose(f);
}

// ---- save states ----

// Build the file path for a slot, keyed by a hash of the mounted disc so each
// game's slots are separate. Creates the directories as needed.
static void
ov_state_path(int slot, char *buf, size_t n)
{
    char dir[PATH_MAX], sub[PATH_MAX];
    ov_config_dir(dir, sizeof dir);

    const char *id = g_current_disc[0] ? g_current_disc : "bios";
    uint32_t h = 5381;
    for (const char *p = id; *p; p++) {
        h = h * 33u + (unsigned char)*p;
    }

    mkdir(dir, 0755);
    SDL_snprintf(sub, sizeof sub, "%s/states", dir);
    mkdir(sub, 0755);
    SDL_snprintf(sub, sizeof sub, "%s/states/%08x", dir, h);
    mkdir(sub, 0755);
    SDL_snprintf(buf, n, "%s/slot%d.sav", sub, slot);
}

static int
ov_slot_exists(int slot)
{
    char path[PATH_MAX];
    struct stat st;
    ov_state_path(slot, path, sizeof path);
    return stat(path, &st) == 0;
}

static void
ov_do_save(int slot)
{
    char path[PATH_MAX];
    ov_state_path(slot, path, sizeof path);
    if (savestate_save(path) == 0) {
        SDL_snprintf(ov.status, sizeof ov.status, "saved to slot %d", slot);
    } else {
        SDL_snprintf(ov.status, sizeof ov.status, "save to slot %d failed", slot);
    }
}

// Returns 1 if the overlay should close (resume the loaded state).
static int
ov_do_load(AppContext *app, int slot)
{
    char path[PATH_MAX];
    ov_state_path(slot, path, sizeof path);
    if (savestate_load(path) == 0) {
        app->have_frame = 0;
        return 1;
    }
    SDL_snprintf(ov.status, sizeof ov.status, "slot %d is empty or invalid", slot);
    return 0;
}

// Run the highlighted menu-bar item. Returns 1 if the overlay should close.
static int
ov_activate(AppContext *app)
{
    if (ov.menu == MENU_FILE) {
        switch (ov.item) {
        case 0: ov_open_picker(PICK_BOOT); return 0;
        case 1: ov_open_picker(PICK_SWAP); return 0;
        case 2: ov.screen = OV_RECENT; ov.sel = 0; ov.scroll = 0; return 0;
        case 3: cdrom_eject(); SDL_snprintf(ov.status, sizeof ov.status, "disc ejected"); return 0;
        case 4: reset_machine(); app->have_frame = 0; return 1;
        case 5: SDL_SetAtomicInt(&app->running, 0); return 1;
        }
    } else if (ov.menu == MENU_STATE) {
        switch (ov.item) {
        case 0: ov.screen = OV_SAVE; ov.sel = 0; return 0;
        case 1: ov.screen = OV_LOAD; ov.sel = 0; return 0;
        }
    } else if (ov.menu == MENU_CARDS) {
        switch (ov.item) {
        case 0: ov_open_picker(PICK_CARD1); return 0;
        case 1: mc_eject_slot(0); SDL_snprintf(ov.status, sizeof ov.status, "card 1 ejected"); return 0;
        case 2: ov_open_picker(PICK_CARD2); return 0;
        case 3: mc_eject_slot(1); SDL_snprintf(ov.status, sizeof ov.status, "card 2 ejected"); return 0;
        }
    } else if (ov.menu == MENU_VIEW) {
        switch (ov.item) {
        case 0: ov_toggle_fullscreen(app); return 0;
        case 1: ov_toggle_vram_view(app); return 0;
        case 2: app->filter_idx ^= 1; ov_apply_filter(app); return 0;
        case 3: app->scale_idx = (app->scale_idx + 1) % OV_SCALE_COUNT; ov_apply_scale(app); return 0;
        case 4: ov_toggle_perf_hud(); return 0;
        }
    } else if (ov.menu == MENU_SETTINGS) {
        switch (ov.item) {
        case 0: ov.screen = OV_MAP; ov.sel = 0; ov.scroll = 0; ov.capturing = 0; return 0;
        case 1: ov_cycle_volume(app); return 0;
        }
    } else if (ov.menu == MENU_HELP) {
        ov.screen = OV_ABOUT;
        return 0;
    }
    return 0;
}

// Neutral navigation actions, shared by the keyboard and the gamepad.
enum { NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_OK, NAV_BACK };

// Returns 1 if the overlay should close.
static int
overlay_nav(AppContext *app, int nav)
{
    if (ov.screen == OV_ABOUT) {
        if (nav == NAV_OK || nav == NAV_BACK) {
            ov.screen = OV_BAR;
        }
        return 0;
    }

    if (ov.screen == OV_PICK) {
        switch (nav) {
        case NAV_UP:
            if (ov.sel > 0) ov.sel--;
            break;
        case NAV_DOWN:
            if (ov.sel < ov.entry_count - 1) ov.sel++;
            break;
        case NAV_OK:
            return ov_pick_choose(app);
        case NAV_BACK:
            ov.screen = OV_BAR;
            break;
        }
        return 0;
    }

    if (ov.screen == OV_RECENT) {
        switch (nav) {
        case NAV_UP:
            if (ov.sel > 0) ov.sel--;
            break;
        case NAV_DOWN:
            if (ov.sel < ov.recent_count - 1) ov.sel++;
            break;
        case NAV_OK:
            return ov_recent_choose(app);
        case NAV_BACK:
            ov.screen = OV_BAR;
            break;
        }
        return 0;
    }

    if (ov.screen == OV_MAP) {
        // Row PAD_BTN_COUNT is the "Reset to Defaults" action.
        switch (nav) {
        case NAV_UP:
            if (ov.sel > 0) ov.sel--;
            break;
        case NAV_DOWN:
            if (ov.sel < PAD_BTN_COUNT) ov.sel++;
            break;
        case NAV_OK:
            if (ov.sel == PAD_BTN_COUNT) {
                keymap_reset_defaults();
                SDL_snprintf(ov.status, sizeof ov.status, "defaults restored");
            } else {
                ov.capturing = 1; // next key press binds this button
                SDL_snprintf(ov.status, sizeof ov.status,
                             "press a key for %s (Esc cancels)",
                             ov_pad_buttons[ov.sel].name);
            }
            break;
        case NAV_BACK:
            ov.screen = OV_BAR;
            break;
        }
        return 0;
    }

    if (ov.screen == OV_SAVE || ov.screen == OV_LOAD) {
        switch (nav) {
        case NAV_UP:
            if (ov.sel > 0) ov.sel--;
            break;
        case NAV_DOWN:
            if (ov.sel < OV_STATE_SLOTS - 1) ov.sel++;
            break;
        case NAV_OK:
            if (ov.screen == OV_SAVE) {
                ov_do_save(ov.sel + 1);
            } else {
                return ov_do_load(app, ov.sel + 1);
            }
            break;
        case NAV_BACK:
            ov.screen = OV_BAR;
            break;
        }
        return 0;
    }

    // OV_BAR
    switch (nav) {
    case NAV_LEFT:
        ov.menu = (ov.menu + OV_MENU_COUNT - 1) % OV_MENU_COUNT;
        ov.item = 0;
        break;
    case NAV_RIGHT:
        ov.menu = (ov.menu + 1) % OV_MENU_COUNT;
        ov.item = 0;
        break;
    case NAV_UP:
        ov.item = (ov.item + ov_menu_item_count[ov.menu] - 1)
                  % ov_menu_item_count[ov.menu];
        break;
    case NAV_DOWN:
        ov.item = (ov.item + 1) % ov_menu_item_count[ov.menu];
        break;
    case NAV_OK:
        return ov_activate(app);
    case NAV_BACK:
        return 1; // close the overlay from the top level
    }
    return 0;
}

static int
overlay_key(AppContext *app, SDL_Keycode key)
{
    // Controller Mapping is waiting to bind the selected button to a key.
    if (ov.capturing) {
        if (key != SDLK_ESCAPE) {
            g_keymap[ov.sel] = key;
            SDL_snprintf(ov.status, sizeof ov.status, "%s = %s",
                         ov_pad_buttons[ov.sel].name, SDL_GetKeyName(key));
        } else {
            SDL_snprintf(ov.status, sizeof ov.status, "rebind cancelled");
        }
        ov.capturing = 0;
        return 0;
    }

    switch (key) {
    case SDLK_UP:     return overlay_nav(app, NAV_UP);
    case SDLK_DOWN:   return overlay_nav(app, NAV_DOWN);
    case SDLK_LEFT:   return overlay_nav(app, NAV_LEFT);
    case SDLK_RIGHT:  return overlay_nav(app, NAV_RIGHT);
    case SDLK_RETURN:
    case SDLK_Z:      return overlay_nav(app, NAV_OK);
    case SDLK_ESCAPE:
    case SDLK_X:      return overlay_nav(app, NAV_BACK);
    default:          return 0;
    }
}

static int
overlay_button(AppContext *app, Uint8 button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP:    return overlay_nav(app, NAV_UP);
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  return overlay_nav(app, NAV_DOWN);
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  return overlay_nav(app, NAV_LEFT);
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return overlay_nav(app, NAV_RIGHT);
    case SDL_GAMEPAD_BUTTON_SOUTH:      return overlay_nav(app, NAV_OK);
    case SDL_GAMEPAD_BUTTON_EAST:       return overlay_nav(app, NAV_BACK);
    default:                            return 0;
    }
}

// ---- drawing (640x480 logical space) ----

#define OV_CH 8 // debug-font character size in pixels

static void
ov_fill(SDL_Renderer *r, int x, int y, int w, int h, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca)
{
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_FRect rect = { (float)x, (float)y, (float)w, (float)h };
    SDL_RenderFillRect(r, &rect);
}

static void
ov_text(SDL_Renderer *r, int x, int y, Uint8 cr, Uint8 cg, Uint8 cb, const char *s)
{
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    SDL_RenderDebugText(r, (float)x, (float)y, s);
}

static void
overlay_draw(AppContext *app)
{
    SDL_Renderer *r = app->renderer;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    ov_fill(r, 0, 0, 640, 480, 0, 0, 0, 150); // dim the frozen frame

    // Menu bar.
    ov_fill(r, 0, 0, 640, 14, 20, 24, 40, 235);
    int x = 8;
    for (int m = 0; m < OV_MENU_COUNT; m++) {
        int w = (int)strlen(ov_menu_titles[m]) * OV_CH + 12;
        if (m == ov.menu) {
            ov_fill(r, x - 4, 0, w, 14, 60, 90, 160, 255);
        }
        ov_text(r, x, 3, 230, 230, 240, ov_menu_titles[m]);
        x += w;
    }

    if (ov.screen == OV_BAR) {
        // Dropdown under the open menu.
        int mx = 8;
        for (int m = 0; m < ov.menu; m++) {
            mx += (int)strlen(ov_menu_titles[m]) * OV_CH + 12;
        }
        int n = ov_menu_item_count[ov.menu];
        int w = 180, rowh = 14;
        ov_fill(r, mx - 4, 14, w, n * rowh + 4, 24, 28, 44, 240);
        for (int i = 0; i < n; i++) {
            int ry = 16 + i * rowh;
            if (i == ov.item) {
                ov_fill(r, mx - 4, ry - 1, w, rowh, 60, 90, 160, 255);
            }
            char label[64];
            ov_item_label(app, ov.menu, i, label, sizeof label);
            ov_text(r, mx, ry, 230, 230, 240, label);
        }
    } else if (ov.screen == OV_RECENT) {
        int px = 40, py = 48, pw = 560, ph = 300;
        ov_fill(r, px, py, pw, ph, 24, 28, 44, 245);
        ov_fill(r, px, py, pw, 16, 40, 48, 72, 255);
        ov_text(r, px + 6, py + 4, 230, 230, 240, "Recent Discs");
        if (ov.recent_count == 0) {
            ov_text(r, px + 12, py + 30, 180, 180, 200, "(none yet)");
        }
        int rowh = 14;
        for (int i = 0; i < ov.recent_count; i++) {
            int ry = py + 30 + i * rowh;
            if (i == ov.sel) {
                ov_fill(r, px + 4, ry - 1, pw - 8, rowh, 60, 90, 160, 255);
            }
            // Show the basename, which is enough to recognize the title.
            const char *base = strrchr(ov.recent[i], '/');
            base = base ? base + 1 : ov.recent[i];
            ov_text(r, px + 8, ry, 230, 230, 240, base);
        }
        ov_text(r, px + 6, py + ph - 12, 150, 150, 170,
                "Up/Down move  Z/Enter boot  X/Esc back");
    } else if (ov.screen == OV_MAP) {
        int px = 160, py = 36, pw = 320, ph = 300, rowh = 15;
        ov_fill(r, px, py, pw, ph, 24, 28, 44, 245);
        ov_fill(r, px, py, pw, 16, 40, 48, 72, 255);
        ov_text(r, px + 6, py + 4, 230, 230, 240, "Controller Mapping");

        int list_y = py + 24;
        for (int i = 0; i < PAD_BTN_COUNT; i++) {
            int ry = list_y + i * rowh;
            if (i == ov.sel) {
                ov_fill(r, px + 4, ry - 1, pw - 8, rowh, 60, 90, 160, 255);
            }
            ov_text(r, px + 10, ry, 210, 210, 230, ov_pad_buttons[i].name);
            const char *kn = (ov.capturing && i == ov.sel)
                             ? "<press a key>" : SDL_GetKeyName(g_keymap[i]);
            ov_text(r, px + 150, ry, 230, 230, 160, kn[0] ? kn : "(unset)");
        }
        int rry = list_y + PAD_BTN_COUNT * rowh + 4;
        if (ov.sel == PAD_BTN_COUNT) {
            ov_fill(r, px + 4, rry - 1, pw - 8, rowh, 60, 90, 160, 255);
        }
        ov_text(r, px + 10, rry, 200, 200, 160, "Reset to Defaults");
        ov_text(r, px + 6, py + ph - 12, 150, 150, 170,
                "Z/Enter rebind  X/Esc back");
    } else if (ov.screen == OV_SAVE || ov.screen == OV_LOAD) {
        int px = 150, py = 40, pw = 340, ph = 220, rowh = 16;
        ov_fill(r, px, py, pw, ph, 24, 28, 44, 245);
        ov_fill(r, px, py, pw, 16, 40, 48, 72, 255);
        ov_text(r, px + 6, py + 4, 230, 230, 240,
                ov.screen == OV_SAVE ? "Save State" : "Load State");
        int list_y = py + 26;
        for (int i = 0; i < OV_STATE_SLOTS; i++) {
            int ry = list_y + i * rowh;
            if (i == ov.sel) {
                ov_fill(r, px + 4, ry - 1, pw - 8, rowh, 60, 90, 160, 255);
            }
            char line[48];
            SDL_snprintf(line, sizeof line, "Slot %d   %s", i + 1,
                         ov_slot_exists(i + 1) ? "saved" : "empty");
            ov_text(r, px + 10, ry, 230, 230, 240, line);
        }
        ov_text(r, px + 6, py + ph - 12, 150, 150, 170,
                ov.screen == OV_SAVE ? "Z/Enter save  X/Esc back"
                                     : "Z/Enter load  X/Esc back");
    } else if (ov.screen == OV_PICK) {
        int px = 40, py = 48, pw = 560, ph = 384;
        ov_fill(r, px, py, pw, ph, 24, 28, 44, 245);
        ov_fill(r, px, py, pw, 16, 40, 48, 72, 255);
        const char *title = (ov.pick_for == PICK_BOOT) ? "Boot Disc"
                          : (ov.pick_for == PICK_SWAP) ? "Swap Disc"
                          : (ov.pick_for == PICK_CARD1) ? "Insert Card 1"
                          : "Insert Card 2";
        ov_text(r, px + 6, py + 4, 230, 230, 240, title);
        ov_text(r, px + 6, py + 22, 150, 170, 210, ov.dir);

        int list_y = py + 40, rowh = 14;
        int rows = (ph - 56) / rowh;
        if (ov.sel < ov.scroll) ov.scroll = ov.sel;
        if (ov.sel >= ov.scroll + rows) ov.scroll = ov.sel - rows + 1;

        for (int i = 0; i < rows; i++) {
            int idx = ov.scroll + i;
            if (idx >= ov.entry_count) break;
            int ry = list_y + i * rowh;
            if (idx == ov.sel) {
                ov_fill(r, px + 4, ry - 1, pw - 8, rowh, 60, 90, 160, 255);
            }
            char line[OV_NAME_MAX + 4];
            SDL_snprintf(line, sizeof line, "%s%s",
                         ov.is_dir[idx] ? "[" : " ", ov.entries[idx]);
            if (ov.is_dir[idx]) {
                SDL_strlcat(line, "]", sizeof line);
            }
            Uint8 g = ov.is_dir[idx] ? 180 : 230;
            ov_text(r, px + 8, ry, g, g, 240, line);
        }
        ov_text(r, px + 6, py + ph - 12, 150, 150, 170,
                "Up/Down move  Z/Enter open  X/Esc back");
    } else if (ov.screen == OV_ABOUT) {
        int px = 140, py = 150, pw = 360, ph = 150;
        ov_fill(r, px, py, pw, ph, 24, 28, 44, 245);
        ov_fill(r, px, py, pw, 16, 40, 48, 72, 255);
        ov_text(r, px + 6, py + 4, 230, 230, 240, "About JonBoy");
        ov_text(r, px + 12, py + 30, 230, 230, 240,
                "JonBoy");
        ov_text(r, px + 12, py + 46, 190, 200, 220,
                "PlayStation 1 (R3000A) emulator in C");
        char line[96];
        int v = SDL_GetVersion();
        SDL_snprintf(line, sizeof line, "SDL %d.%d.%d",
                     SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
                     SDL_VERSIONNUM_MICRO(v));
        ov_text(r, px + 12, py + 70, 190, 200, 220, line);
        ov_text(r, px + 12, py + ph - 14, 150, 150, 170, "Z/Enter or X/Esc to close");
    }

    // Status line along the bottom.
    if (ov.status[0]) {
        ov_fill(r, 0, 466, 640, 14, 20, 24, 40, 235);
        ov_text(r, 8, 469, 200, 220, 160, ov.status);
    }
}

int SDLCALL
jonboy_runapp(int argc, char *argv[])
{
    (void)argc, (void)argv;

    // 1. Initialize SDL3 subsystems
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init Error: %s", SDL_GetError());
        return 1;
    }

    AppContext app = { 0 };
    SDL_SetAtomicInt(&app.running, 1);

    // Defaults, then the saved config (jonboy.ini) overrides them, then JONBOY_PERF
    // forces the HUD on for this run if set. These populate app fields, the keyboard
    // map, and the recent list; the display and audio values are realized once the
    // window, renderer, and audio stream exist below.
    app.scale_idx = 1;   // 960x720, matching the window created below
    app.volume = 100;
    ov_config_load(&app);
    if (getenv("JONBOY_PERF") != NULL) {
        SDL_SetAtomicInt(&g_perf_hud, 1);
    }
    if (g_cd_path) {
        SDL_strlcpy(g_current_disc, g_cd_path, sizeof g_current_disc); // for state slots
    }

    // 2. Create an SDL3 window and hardware-accelerated renderer. The PSX display
    // is 4:3 on a TV whatever its internal pixel resolution, so the window opens
    // at a 4:3 size and is resizable.
    if (!SDL_CreateWindowAndRenderer("JonBoy PSX Emulator", 960, 720,
                                     SDL_WINDOW_RESIZABLE, &app.window, &app.renderer)) {
        SDL_Log("Window/Renderer Creation Failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // The PSX picture is always the 4:3 FULL format on a TV regardless of its
    // internal horizontal resolution, so present into a fixed 640x480 (FULL) logical
    // area. The SDL "letterbox" presentation is only the fit strategy: it adds bars
    // when the output is not 4:3. Windowed, the window is locked to 4:3 below so no
    // bars ever show; fullscreen pillar/letterboxes the FULL picture inside the
    // native display. Everything below draws to the whole logical area (NULL dest).
    SDL_SetRenderLogicalPresentation(app.renderer, 640, 480,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetWindowAspectRatio(app.window, 4.0f / 3.0f, 4.0f / 3.0f);
    if (app.fullscreen) {
        SDL_SetWindowFullscreen(app.window, true);
    } else {
        ov_apply_scale(&app);
    }

    // Pace presentation to the display refresh so the main thread does not spin.
    // The emulation thread keeps its own real-time pacing independently.
    SDL_SetRenderVSync(app.renderer, 1);

    // Triple-buffer handoff: the three buffers start partitioned as producer(0),
    // shared(2), consumer(1). Every publish/consume swaps a private index with
    // the shared slot, so this partition is preserved and the two threads never
    // touch the same buffer.
    app.produce_idx = 0;
    app.consume_idx = 1;
    SDL_SetAtomicInt(&app.frame_slot, 2);

    // CREATE THE VRAM UPLOAD TEXTURE:
    // PSX VRAM pixels are 5-5-5: bit15 = mask/STP, bits14-10 = blue,
    // bits9-5 = green, bits4-0 = red. That layout is XBGR1555. Using a 565
    // format here shifts the green and blue channels (green steals blue's low
    // bit, blue steals the mask bit), which tints grays pink and blues green.
    // SDL_TEXTUREACCESS_STREAMING allows us to dynamically lock and overwrite pixels every frame.
    app.vram_texture = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_XBGR1555, SDL_TEXTUREACCESS_STREAMING, VRAM_WIDTH, VRAM_HEIGHT);

    if (!app.vram_texture) {
        SDL_Log("Failed to create VRAM texture: %s", SDL_GetError());
        SDL_DestroyRenderer(app.renderer);
        SDL_DestroyWindow(app.window);
        SDL_Quit();
        return 1;
    }

    // A second target for 24-bit display mode (full-motion video). VRAM holds
    // packed 24-bit RGB there, which the 15-bit texture above cannot represent,
    // so present_frame unpacks the display area into this one instead. Sized to
    // the widest display area a 24-bit frame can occupy.
    app.disp24_texture = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_RGB24,
                                           SDL_TEXTUREACCESS_STREAMING,
                                           VRAM_WIDTH, VRAM_HEIGHT);

    if (!app.disp24_texture) {
        SDL_Log("Failed to create 24-bit display texture: %s", SDL_GetError());
        SDL_DestroyTexture(app.vram_texture);
        SDL_DestroyRenderer(app.renderer);
        SDL_DestroyWindow(app.window);
        SDL_Quit();
        return 1;
    }

    // Both display textures exist now, so realize the saved scaling filter.
    ov_apply_filter(&app);

    // Open the audio output stream for SPU/CD sound. A failure here is not
    // fatal: the emulator runs silently.
    // Let SDL put its audio callback thread on the real-time scheduler. SDL already
    // runs that thread TIME_CRITICAL; this hint promotes it to SCHED_RR on Linux
    // (through rtkit over D-Bus, so no elevated privileges are needed). The callback
    // does only a small, bounded ring copy, so it stays well inside rtkit's runtime
    // budget. Where rtkit or the permission is absent the request fails and the
    // thread stays on the normal scheduler, which the cushion still covers. Must be
    // set before the device opens.
    SDL_SetHint(SDL_HINT_THREAD_FORCE_REALTIME_TIME_CRITICAL, "1");

    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, AUDIO_RATE };
    app.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, audio_callback, NULL);
    if (app.audio) {
        // Set the gain now, but leave the device paused. The emu thread resumes it
        // once it has first filled the ring to the cushion, so the device never
        // drains an empty ring at startup (which would be an audible opening gap).
        ov_apply_volume(&app); // the configured master volume
    } else {
        SDL_Log("Audio open failed, running silent: %s", SDL_GetError());
    }

    // 3. Load the BIOS image into the mapped BIOS region (0x1FC00000).
    // A missing BIOS is not fatal during development: the window still opens,
    // the CPU just runs zeroed ROM.
    if (mem_load_bios(g_bios_path) == 0) {
        printf("BIOS loaded.\n");
    } else {
        printf("No BIOS loaded, continuing with blank ROM.\n");
    }

    // Optionally load PSIO firmware, mount a virtual SD card, and patch the
    // BIOS entry so the CPU boots into the PSIO firmware instead of the BIOS.
    if (g_psio_firmware) {
        load_psio_firmware(g_psio_firmware);
    }

    if (g_psio_sd) {
        load_virtual_sd_card(g_psio_sd);
    }

    if (g_psio_boot) {
        inject_boot_stub(bios_rom);
        printf("PSIO boot stub injected.\n");
    }

    if (g_memcard_path && mc_mount_slot(0, g_memcard_path) == 0) {
        printf("Memory card (slot 1) %s\n", g_memcard_path);
    }
    if (g_memcard2_path && mc_mount_slot(1, g_memcard2_path) == 0) {
        printf("Memory card (slot 2) %s\n", g_memcard2_path);
    }

    if (g_games_dir) {
        loader_set_games_dir(g_games_dir);
    }

    if (g_cd_path) {
        if (cdrom_mount(g_cd_path) == 0) {
            printf("Mounted disc %s\n", g_cd_path);
        } else {
            fprintf(stderr, "cd: failed to mount %s\n", g_cd_path);
        }
    }

    // Start execution at the CPU reset vector (BIOS entry point).
    cpu_reset(0xBFC00000);
    // Real-time build: batch the per-step hardware tick and interrupt poll for
    // throughput. The deterministic run modes (headless, exe, boot, gdb) leave
    // this off so conformance and the boot regression stay bit-exact.
    cpu_set_fasttick(CPU_FASTTICK_DEFAULT);

    // Prefetch CD sectors on a worker thread so a slow disc image (for example
    // one on a network mount) never blocks input and rendering on an fread. Only
    // the windowed path does this; the deterministic run modes read synchronously.
    cdrom_io_start();

    // GPU worker: GP0 rasterization runs on this thread in parallel with the
    // emulation thread (docs/threaded-gpu.md). On by default in the windowed build;
    // the deterministic run modes never reach here and stay synchronous. Set
    // JONBOY_GPU_THREAD=0 to opt out (fall back to inline rasterization). Start it
    // before the emu thread so the first submitted commands have a consumer.
    const char *gpu_thread_env = getenv("JONBOY_GPU_THREAD");
    gpu_threaded = (gpu_thread_env == NULL) ? 1 : (atoi(gpu_thread_env) != 0);
    if (gpu_threaded) {
        gpu_worker_reset();
        app.gpu_worker = SDL_CreateThread(gpu_worker_thread, "jonboy-gpu", &app);
        if (!app.gpu_worker) {
            SDL_Log("Failed to start GPU worker: %s", SDL_GetError());
            gpu_threaded = 0; // fall back to synchronous rasterization
        } else {
            SDL_Log("GPU worker thread enabled");
        }
    }

    // 4. Start the emulation thread. It runs the deterministic core; this main
    // thread stays free to pump events, sample input, and present frames, so a
    // slow present or a blocked event pump never stalls emulation or audio.
    app.emu = SDL_CreateThread(emu_thread, "jonboy-emu", &app);
    if (!app.emu) {
        SDL_Log("Failed to start emulation thread: %s", SDL_GetError());
        SDL_SetAtomicInt(&app.running, 0);
    }

    // 5. Main thread: events, input, and presentation.
    uint16_t kbd_buttons = 0xFFFF; // keyboard button mask, active low

    while (SDL_GetAtomicInt(&app.running)) {
        SDL_Event event;

        // Handle window closure events and keyboard escapes
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                SDL_SetAtomicInt(&app.running, 0);
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (SDL_GetAtomicInt(&app.overlay)) {
                    // The menu consumes input; none of it reaches the pad.
                    if (overlay_key(&app, event.key.key)) {
                        overlay_close(&app);
                    }
                } else if (event.key.key == SDLK_ESCAPE) {
                    overlay_open(&app); // Esc opens the menu (Exit quits from it)
                } else if (event.key.key == SDLK_F1) {
                    app.full_vram_view = !app.full_vram_view;
                    // Tell the emu thread to publish the whole VRAM snapshot the
                    // debug view needs, instead of just the display region.
                    SDL_SetAtomicInt(&g_full_vram, app.full_vram_view);
                } else if (event.key.key == SDLK_TAB && !event.key.repeat) {
                    pad_toggle_analog(); // ANALOG button
                    printf("Pad analog mode %s\n",
                           pad_analog_mode() ? "on" : "off");
                }

                if (!SDL_GetAtomicInt(&app.overlay)) {
                    uint16_t bit = pad_bit_for_key(event.key.key);
                    if (bit) {
                        kbd_buttons &= ~bit; // active low: pressed clears the bit
                    }
                }
            } else if (event.type == SDL_EVENT_KEY_UP) {
                // Always release, even if the key went down before the menu opened,
                // so no pad bit stays stuck once the game resumes.
                uint16_t bit = pad_bit_for_key(event.key.key);
                if (bit) {
                    kbd_buttons |= bit; // released sets the bit
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                if (!app.gamepad) {
                    app.gamepad = SDL_OpenGamepad(event.gdevice.which);
                    if (app.gamepad) {
                        printf("Gamepad connected: %s\n",
                               SDL_GetGamepadName(app.gamepad));
                    }
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                if (app.gamepad &&
                    event.gdevice.which == SDL_GetGamepadID(app.gamepad)) {
                    SDL_CloseGamepad(app.gamepad);
                    app.gamepad = NULL;
                    printf("Gamepad disconnected\n");
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                if (SDL_GetAtomicInt(&app.overlay)) {
                    if (overlay_button(&app, event.gbutton.button)) {
                        overlay_close(&app);
                    }
                } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
                    overlay_open(&app); // the Guide/Home button opens the menu
                }
            }
        }

        // Fold this frame's keyboard and gamepad state into the pad, unless a
        // script is driving the controller (it sets the pad state at VBLANK) or the
        // menu overlay is open (the game is frozen and must see a neutral pad). The
        // emu thread reads these globals; the values are single words, so a torn
        // read is harmless (at worst one frame of mixed input).
        if (!script_active() && !SDL_GetAtomicInt(&app.overlay)) {
            update_pad_input(app.gamepad, kbd_buttons);
        }

        // Render the latest published frame, draw the menu over it when open, then
        // present once. VSync paces this loop; SDL_Delay is a fallback when vsync is
        // unavailable (a dummy video driver).
        render_game_frame(&app);
        if (SDL_GetAtomicInt(&app.overlay)) {
            overlay_draw(&app);
        }
        SDL_RenderPresent(app.renderer);
        SDL_Delay(1);
    }

    // 6. Clean Cleanup Strategy: stop the emu thread before tearing anything down.
    SDL_SetAtomicInt(&app.running, 0);
    ov_config_save(&app); // persist settings even if quit bypassed the menu
    if (app.emu) {
        SDL_WaitThread(app.emu, NULL);
    }
    // Stop the GPU worker only after the emu thread has joined, so no more GP0
    // commands are submitted while it drains and exits.
    if (app.gpu_worker) {
        gpu_worker_request_stop();
        SDL_WaitThread(app.gpu_worker, NULL);
    }
    cdrom_io_stop();
    if (app.gamepad) {
        SDL_CloseGamepad(app.gamepad);
    }
    if (app.audio) {
        SDL_DestroyAudioStream(app.audio);
    }
    SDL_DestroyTexture(app.vram_texture);
    SDL_DestroyTexture(app.disp24_texture);
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    SDL_Quit();

    return 0;
}

// ============================================================================
// HEADLESS TEST HARNESS (Milestone 0)
// ============================================================================
// Runs a flat binary with no window: load it into RAM at a physical address,
// run up to a step limit or until the CPU halts (BREAK / SYSCALL), then dump
// register and memory state so a test ROM can report pass/fail.

// Canonical MIPS register mnemonics, indexed by register number.
static const char *reg_names[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

static void
dump_registers(void)
{
    printf("PC = 0x%08X\n", cpu.PC);

    for (int i = 0; i < 32; i += 4) {
        for (int j = 0; j < 4; j++) {
            int r = i + j;
            printf("  $%-4s(r%-2d)=0x%08X", reg_names[r], r, cpu.regs[r]);
        }

        printf("\n");
    }

    // $s0 (r16) is the conventional result register for the bundled self-test.
    printf("RESULT $s0 = 0x%08X\n", cpu.regs[16]);
}

static void
dump_memory(uint32_t addr, uint32_t words)
{
    printf("Memory dump @ 0x%08X (%u words):\n", addr, words);

    for (uint32_t i = 0; i < words; i++) {
        if ((i % 4) == 0) {
            printf("  0x%08X:", addr + i * 4);
        }

        printf(" %08X", memory_read_word(addr + i * 4));

        if ((i % 4) == 3) {
            printf("\n");
        }
    }

    if ((words % 4) != 0) {
        printf("\n");
    }
}

static void dump_vram_ppm(const char *path); // defined below

static int
run_headless(const char *path, uint32_t load_addr, uint32_t start_pc,
             uint64_t max_steps, const char *vram_out)
{
    if (mem_load_file(path, load_addr) != 0) {
        fprintf(stderr, "headless: failed to load %s\n", path);
        return 1;
    }

    printf("Loaded %s at 0x%08X, entry PC 0x%08X, step limit %llu\n",
           path, load_addr, start_pc, (unsigned long long)max_steps);

    cpu_reset(start_pc);
    cpu.halt_on_break = 1; // let a test use BREAK to stop and report

    uint64_t steps = 0;
    uint64_t start_counter = SDL_GetPerformanceCounter();

    while (!cpu.halted && steps < max_steps) {
        step_cpu(); // interrupts are recognized inside step_cpu
        steps++;
    }

    if (cpu.halted) {
        printf("\nCPU halted after %llu steps.\n", (unsigned long long)steps);
    } else {
        printf("\nStep limit reached after %llu steps (no halt).\n",
               (unsigned long long)steps);
    }

    report_realtime(steps, start_counter);

    dump_registers();
    dump_memory(load_addr, 16);

    if (vram_out) {
        dump_vram_ppm(vram_out);
    }

    return 0;
}

// ============================================================================
// PS-EXE CONFORMANCE RUNNER (Milestone 2)
// ============================================================================
// Loads a PlayStation executable and runs it headless. The PeterLemon CPU
// tests draw a green "PASS" or red "FAIL" glyph per subtest into VRAM, so the
// verdict is read back by sampling the colors in the result column.

// Convert one BGR555 VRAM pixel into 8-bit R, G, B components.
static void
bgr555_to_rgb(uint16_t px, int *r, int *g, int *b)
{
    int r5 = px & 0x1F;
    int g5 = (px >> 5) & 0x1F;
    int b5 = (px >> 10) & 0x1F;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g5 << 3) | (g5 >> 2);
    *b = (b5 << 3) | (b5 >> 2);
}

// Dump the visible 320x240 display area of VRAM as a binary PPM, decoded from
// the GPU's display origin and honouring 24-bit colour mode (used by full-motion
// video). Setting the VRAMFULL environment variable dumps the entire 1024x512
// VRAM as raw 15-bit instead, which is needed to inspect off-screen texture
// pages such as the shell flame at (896,0).
static void
dump_vram_ppm_impl(const char *path, int quiet)
{
    FILE *f = fopen(path, "wb");

    if (!f) {
        perror(path);
        return;
    }

    int full = getenv("VRAMFULL") != NULL;
    int w = full ? VRAM_WIDTH : 320;
    int h = full ? VRAM_HEIGHT : 240;
    fprintf(f, "P6\n%d %d\n255\n", w, h);

    int ox = full ? 0 : gpu.disp_x;
    int oy = full ? 0 : gpu.disp_y;
    // Pixels the display range does not cover hold stale VRAM; the windowed
    // build hides them, so blank them here too instead of dumping garbage. In
    // the full-VRAM view every pixel is shown as-is.
    int active_h = full ? h : psx_gpu_active_height();
    int active_w = full ? w : psx_gpu_active_width();

    for (int y = 0; y < h; y++) {
        int row = (oy + y) & 0x1FF;
        for (int x = 0; x < w; x++) {
            unsigned char rgb[3];
            if (y >= active_h || x >= active_w) {
                rgb[0] = rgb[1] = rgb[2] = 0;
            } else if (!full && gpu.disp_24bpp) {
                // 24-bit display: three packed bytes per pixel across the
                // 16-bit VRAM words. The horizontal origin is in VRAM columns.
                int b0 = ox * 2 + x * 3;
                int c0 = (b0 >> 1) & 0x3FF;
                int c1 = ((b0 + 1) >> 1) & 0x3FF;
                int c2 = ((b0 + 2) >> 1) & 0x3FF;
                uint16_t w0 = vram.canvas[c0][row];
                uint16_t w1 = vram.canvas[c1][row];
                uint16_t w2 = vram.canvas[c2][row];
                rgb[0] = (b0 & 1) ? (w0 >> 8) : (w0 & 0xFF);
                rgb[1] = ((b0 + 1) & 1) ? (w1 >> 8) : (w1 & 0xFF);
                rgb[2] = ((b0 + 2) & 1) ? (w2 >> 8) : (w2 & 0xFF);
            } else {
                int r, g, b;
                int col = full ? x : (ox + x) & 0x3FF;
                bgr555_to_rgb(vram.canvas[col][row], &r, &g, &b);
                rgb[0] = (unsigned char)r;
                rgb[1] = (unsigned char)g;
                rgb[2] = (unsigned char)b;
            }
            fwrite(rgb, 1, 3, f);
        }
    }

    fclose(f);
    if (!quiet) {
        printf("Wrote VRAM image to %s\n", path);
    }
}

static void
dump_vram_ppm(const char *path)
{
    dump_vram_ppm_impl(path, 0);
}

// Periodic-screenshot debug aid. When --frames-out is set, the boot loop writes
// a numbered framebuffer PPM every N video frames from a start frame on, so a
// single headless run produces a filmstrip. This makes it practical to see and
// choreograph timed pad input (menus, naming screens, cutscenes) that one
// end-of-run screenshot cannot show.
static const char *g_frames_dir = NULL; // directory for periodic framebuffer dumps
static int g_frames_every = 30;         // dump one frame every this many video frames
static uint32_t g_frames_from = 0;      // first video frame to start dumping at

static void
maybe_dump_frame(uint64_t steps)
{
    static long last_dumped = -1;

    if (!g_frames_dir) {
        return;
    }

    long frame = (long)(steps / STEPS_PER_FRAME);

    if (frame == last_dumped || frame < (long)g_frames_from
        || (frame % g_frames_every) != 0) {
        return;
    }

    last_dumped = frame;

    char path[1024];
    snprintf(path, sizeof path, "%s/frame%06ld.ppm", g_frames_dir, frame);
    dump_vram_ppm_impl(path, 1);
}

// Count green (PASS) and red (FAIL) glyph pixels in the result column of the
// PeterLemon CPU tests (x in [280,320), below the header row).
static void
classify_results(int *out_green, int *out_red)
{
    int green = 0, red = 0;

    for (int y = 24; y < 240; y++) {
        for (int x = 280; x < 320; x++) {
            int r, g, b;
            bgr555_to_rgb(vram.canvas[x][y], &r, &g, &b);

            if (g > r + 32 && g > b + 32) {
                green++;
            } else if (r > g + 32 && r > b + 32) {
                red++;
            }
        }
    }

    *out_green = green;
    *out_red = red;
}

static int
run_exe(const char *path, uint64_t max_steps, const char *vram_out)
{
    uint32_t pc, gp, sp;

    if (mem_load_exe(path, &pc, &gp, &sp) != 0) {
        return 1;
    }

    cpu_reset(pc);
    cpu.regs[28] = gp; // $gp
    cpu.regs[29] = sp; // $sp
    cpu.regs[30] = sp; // $fp
    cpu.halt_on_break = 1;

    printf("Running %s: PC=0x%08X GP=0x%08X SP=0x%08X, step limit %llu\n",
           path, pc, gp, sp, (unsigned long long)max_steps);

    uint64_t steps = 0;
    uint64_t start_counter = SDL_GetPerformanceCounter();

    while (!cpu.halted && steps < max_steps) {
        step_cpu();
        steps++;
    }

    printf("Ran %llu steps (PC=0x%08X).\n", (unsigned long long)steps, cpu.PC);
    report_realtime(steps, start_counter);

    int green = 0, red = 0;
    classify_results(&green, &red);
    printf("Result column: %d green (PASS) pixels, %d red (FAIL) pixels\n", green, red);

    if (vram_out) {
        dump_vram_ppm(vram_out);
    }

    if (green > 0 && red == 0) {
        printf("VERDICT: PASS\n");
        return 0;
    }

    printf("VERDICT: FAIL\n");
    return 2;
}

// Headless boot: load the BIOS (if present), optionally patch it with the PSIO
// stub, reset to the boot vector, run, and dump registers. Lets the boot path
// be exercised and inspected without a window.
static int
run_boot(int psio, const char *sd, const char *firmware, uint64_t max_steps,
         const char *vram_out)
{
    if (mem_load_bios(g_bios_path) == 0) {
        printf("BIOS loaded.\n");
    } else {
        printf("No BIOS loaded, continuing with blank ROM.\n");
    }

    if (firmware) {
        load_psio_firmware(firmware);
    }

    if (sd) {
        load_virtual_sd_card(sd);
    }

    if (psio) {
        inject_boot_stub(bios_rom);
        printf("PSIO boot stub injected.\n");
    }

    if (g_games_dir) {
        loader_set_games_dir(g_games_dir);
    }

    if (g_cd_path) {
        if (cdrom_mount(g_cd_path) == 0) {
            printf("Mounted disc %s\n", g_cd_path);
        } else {
            fprintf(stderr, "cd: failed to mount %s\n", g_cd_path);
        }
    }

    if (g_memcard_path && mc_mount_slot(0, g_memcard_path) == 0) {
        printf("Memory card (slot 1) %s\n", g_memcard_path);
    }
    if (g_memcard2_path && mc_mount_slot(1, g_memcard2_path) == 0) {
        printf("Memory card (slot 2) %s\n", g_memcard2_path);
    }

    cpu_reset(0xBFC00000);
    // A sideloaded program runs with a live BIOS, so BREAK must vector to the
    // handler (some tests rely on it) rather than stopping the harness.
    cpu.halt_on_break = g_sideload_exe ? 0 : 1;

    uint64_t steps = 0;

    // When sideloading, run the BIOS until it hands control to the shell entry
    // (0x80030000), by which point the kernel is fully initialized. Then load
    // the PS-EXE over RAM and jump to its entry, so it runs with a live BIOS.
    int sideloaded = 0;
    int kernel_up = 0; // set once the BIOS reaches the shell entry point
    uint64_t start_counter = SDL_GetPerformanceCounter();

    // Skip the per-step loader/sideload service and the frame-dump check when
    // neither is configured, so a plain boot pays only a predicted branch per
    // instruction instead of two function calls.
    int service_active = (g_sideload_exe != NULL) || (g_games_dir != NULL);
    int dumping = (g_frames_dir != NULL);

    while (!cpu.halted && steps < max_steps) {
        if (service_active) {
            service_loader_boot(&kernel_up, &sideloaded);
        }
        step_cpu();
        steps++;
        if (dumping) {
            maybe_dump_frame(steps);
        }
    }

    printf("Ran %llu steps.\n", (unsigned long long)steps);
    report_realtime(steps, start_counter);
    dump_registers();
    // Show the instructions around where execution ended, to spot poll loops.
    dump_memory((cpu.PC - 16) & ~0xFu, 8);

    // Report MDEC activity: a nonzero colour macroblock count means the game
    // ran FMV frames through the decoder during this boot.
    {
        uint32_t decodes, color_mb, mono_blk;
        mdec_get_stats(&decodes, &color_mb, &mono_blk);
        printf("MDEC: %u decode commands, %u colour macroblocks, %u mono blocks\n",
               decodes, color_mb, mono_blk);
    }
    {
        uint64_t cd_in, cd_over, cd_under;
        spu_cd_get_stats(&cd_in, &cd_over, &cd_under);
        printf("CD/XA audio: %llu frames fed, %llu dropped (FIFO full), "
               "%llu drains starved (FIFO empty mid-stream)\n",
               (unsigned long long)cd_in, (unsigned long long)cd_over,
               (unsigned long long)cd_under);
    }

    if (vram_out) {
        dump_vram_ppm(vram_out);
    }

    return 0;
}

// qsort comparator for the per-frame time series (ascending).
static int
bench_cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

// Benchmark mode: measure the wall time the emulation core spends per emulated
// video frame on a real workload. This is the number the real-time budget lives
// or dies by. A PS1 frame is 1/60 s = 16.67 ms, so when the mean (or a p99 spike)
// approaches that, there is no slack left for the windowed build's present and
// audio underruns. The core runs unpaced and single-threaded here, so the exact
// same instructions execute every run; the measured wall times still vary a
// little with host scheduling, so mean and median are the stable figures to
// compare across optimizations and the p99/max tail is scheduling-sensitive. The
// audio and video drop counts are derived from the per-frame series with two
// small, explicit models, so they need no real device.
//
// What is measured is the emulation thread's own work: CPU interpretation, GPU
// rasterization, SPU mixing, DMA, and CD. The windowed build's VRAM publish copy
// and the present-time transpose are separate main-thread work and are not here.
static int
run_bench(uint32_t warmup_frames, uint32_t measure_frames)
{
    // Devices: the same setup as run_boot, so --bios/--cd/--script/--memcard all
    // select the workload.
    if (mem_load_bios(g_bios_path) == 0) {
        printf("BIOS loaded.\n");
    } else {
        printf("No BIOS loaded, continuing with blank ROM.\n");
    }

    if (g_psio_firmware) {
        load_psio_firmware(g_psio_firmware);
    }
    if (g_psio_sd) {
        load_virtual_sd_card(g_psio_sd);
    }
    if (g_psio_boot) {
        inject_boot_stub(bios_rom);
        printf("PSIO boot stub injected.\n");
    }
    if (g_games_dir) {
        loader_set_games_dir(g_games_dir);
    }
    if (g_cd_path) {
        if (cdrom_mount(g_cd_path) == 0) {
            printf("Mounted disc %s\n", g_cd_path);
        } else {
            fprintf(stderr, "cd: failed to mount %s\n", g_cd_path);
        }
    }
    if (g_memcard_path && mc_mount_slot(0, g_memcard_path) == 0) {
        printf("Memory card (slot 1) %s\n", g_memcard_path);
    }
    if (g_memcard2_path && mc_mount_slot(1, g_memcard2_path) == 0) {
        printf("Memory card (slot 2) %s\n", g_memcard2_path);
    }

    cpu_reset(0xBFC00000);
    cpu.halt_on_break = g_sideload_exe ? 0 : 1;
    // Measure the real-time runtime, which batches the hardware tick, so the
    // benchmark reflects the windowed build. JONBOY_FASTTICK still overrides.
    cpu_set_fasttick(CPU_FASTTICK_DEFAULT);

    int service_active = (g_sideload_exe != NULL) || (g_games_dir != NULL);
    int kernel_up = 0, sideloaded = 0;

    // ms[] is the per-frame emulation-thread budget (step plus the publish copy
    // the windowed emu thread does at end of frame). tr[] is the row-major
    // transpose the windowed main thread does to present; it is reported on its
    // own, not folded into the budget, because it runs on a different thread with
    // its own parallel budget and does not compete with audio. bench_pub and
    // bench_row are the scratch buffers the publish and transpose write into, so
    // the benchmark does the identical work the windowed build does.
    double *ms = malloc((size_t)measure_frames * sizeof *ms);
    double *tr = malloc((size_t)measure_frames * sizeof *tr);
    // raster[] is the per-frame time spent dispatching GP0 primitives, a subset
    // of the budget: it is the work a GPU worker thread would run instead, so it
    // bounds the threaded-GPU payoff (step 0 of docs/threaded-gpu.md).
    double *raster = malloc((size_t)measure_frames * sizeof *raster);
    uint16_t *bench_pub = malloc((size_t)VRAM_WIDTH * VRAM_HEIGHT * sizeof(uint16_t));
    uint16_t *bench_row = malloc((size_t)VRAM_WIDTH * VRAM_HEIGHT * sizeof(uint16_t));
    if (!ms || !tr || !raster || !bench_pub || !bench_row) {
        fprintf(stderr, "bench: out of memory\n");
        free(ms);
        free(tr);
        free(raster);
        free(bench_pub);
        free(bench_row);
        return 1;
    }

    double freq = (double)SDL_GetPerformanceFrequency();

    printf("Benchmark: warming up %u frames, then measuring %u...\n",
           warmup_frames, measure_frames);

    // Warmup: run (unmeasured) past the BIOS boot and into the workload proper.
    while (!cpu.halted && psx_frame_count() < warmup_frames) {
        if (service_active) {
            service_loader_boot(&kernel_up, &sideloaded);
        }
        step_cpu();
    }

    // Measure: record the wall time between VBLANKs. A frame counter that jumps
    // by more than one (never, with the per-step tick, but defensively) splits
    // its wall time evenly across the frames it covers.
    gpu_profile_enable(1); // accumulate GP0 primitive-dispatch time per frame
    uint32_t recorded = 0;
    uint32_t last_frame = psx_frame_count();
    uint64_t raster_prev = gpu_profile_ns();
    uint64_t t_prev = SDL_GetPerformanceCounter();
    while (!cpu.halted && recorded < measure_frames) {
        if (service_active) {
            service_loader_boot(&kernel_up, &sideloaded);
        }
        step_cpu();
        uint32_t f = psx_frame_count();
        if (f != last_frame) {
            uint64_t now = SDL_GetPerformanceCounter();
            double step_ms = (double)(now - t_prev) / freq * 1000.0;
            uint64_t raster_now = gpu_profile_ns();
            double raster_ms = (double)(raster_now - raster_prev) / 1.0e6;
            raster_prev = raster_now;
            uint32_t span = f - last_frame;

            // Do the same end-of-frame work the windowed build does, on the live
            // VRAM: the publish copy (part of the emu-thread budget) and the
            // present transpose (main-thread, measured apart). Using the shared
            // helpers keeps the copied region size identical to the real build.
            int dx, dy, dw, dh, blank, bpp24, full;
            frame_display_region(&dx, &dy, &dw, &dh, &blank, &bpp24, &full);

            uint64_t p0 = SDL_GetPerformanceCounter();
            publish_vram_region(bench_pub, dx, dy, dw, dh, full);
            uint64_t p1 = SDL_GetPerformanceCounter();
            if (full) {
                transpose_full_rowmajor(bench_row, bench_pub);
            } else {
                transpose_region_rowmajor(bench_row, bench_pub, dw, dh);
            }
            uint64_t p2 = SDL_GetPerformanceCounter();

            double pub_ms = (double)(p1 - p0) / freq * 1000.0;
            double tr_ms = (double)(p2 - p1) / freq * 1000.0;

            for (uint32_t j = 0; j < span && recorded < measure_frames; j++) {
                ms[recorded] = (step_ms + pub_ms) / (double)span;
                tr[recorded] = tr_ms / (double)span;
                raster[recorded] = raster_ms / (double)span;
                recorded++;
            }
            // Reset after the publish/transpose so their time is not charged to
            // the next frame's step measurement.
            t_prev = SDL_GetPerformanceCounter();
            last_frame = f;
        }
    }
    gpu_profile_enable(0);

    if (recorded == 0) {
        fprintf(stderr, "bench: no frames emulated (did the CPU halt during "
                "warmup?)\n");
        free(ms);
        free(tr);
        free(raster);
        free(bench_pub);
        free(bench_row);
        return 1;
    }

    // CPU-per-frame statistics.
    double sum = 0.0, worst = 0.0;
    for (uint32_t i = 0; i < recorded; i++) {
        sum += ms[i];
        if (ms[i] > worst) {
            worst = ms[i];
        }
    }
    double mean = sum / recorded;

    double *sorted = malloc((size_t)recorded * sizeof *sorted);
    if (!sorted) {
        fprintf(stderr, "bench: out of memory\n");
        free(ms);
        free(tr);
        free(raster);
        free(bench_pub);
        free(bench_row);
        return 1;
    }
    memcpy(sorted, ms, (size_t)recorded * sizeof *sorted);
    qsort(sorted, recorded, sizeof *sorted, bench_cmp_double);
    double median = sorted[recorded / 2];
    double p99 = sorted[(uint32_t)((double)(recorded - 1) * 0.99)];

    const double budget = 1000.0 / TARGET_FPS; // 16.67 ms real-time per frame

    uint32_t over = 0;
    for (uint32_t i = 0; i < recorded; i++) {
        if (ms[i] > budget) {
            over++;
        }
    }

    double total_ms = sum;
    double sustained_fps = (double)recorded / (total_ms / 1000.0);
    double realtime = (recorded * budget) / total_ms;

    // Audio model: a ring that holds spu_output_capacity() frames of 44.1 kHz
    // audio absorbs per-frame spikes. Each emulated frame adds one frame's worth
    // (budget ms) of audio but takes ms[i] of wall time to make, during which the
    // device drains ms[i] of audio. Net change is (budget - ms[i]). Start full
    // (the best case for an isolated window) and count a starvation each time it
    // empties.
    double ring_cap = 1000.0 * spu_output_capacity() / AUDIO_RATE;
    double ring = ring_cap;
    uint32_t starved = 0, underrun_events = 0;
    double gap_ms = 0.0;
    int was_empty = 0;
    for (uint32_t i = 0; i < recorded; i++) {
        ring += budget - ms[i];
        if (ring <= 0.0) {
            gap_ms += -ring;
            ring = 0.0;
            starved++;
            if (!was_empty) {
                underrun_events++;
            }
            was_empty = 1;
        } else {
            was_empty = 0;
            if (ring > ring_cap) {
                ring = ring_cap;
            }
        }
    }

    // Video model: a 60 Hz presenter wants one frame per budget ms of wall time.
    // Over the wall time this window took, real-time would show total_ms/budget
    // frames; the core produced `recorded`. The shortfall is dropped (repeated)
    // frames the player sees as stutter.
    double want_frames = total_ms / budget;
    double dropped = want_frames > recorded ? want_frames - recorded : 0.0;

    // Present-overhead (transpose) statistics. Reuse the sorted scratch, since the
    // core percentiles above are already extracted into scalars.
    double tr_sum = 0.0, tr_worst = 0.0;
    for (uint32_t i = 0; i < recorded; i++) {
        tr_sum += tr[i];
        if (tr[i] > tr_worst) {
            tr_worst = tr[i];
        }
    }
    double tr_mean = tr_sum / recorded;
    memcpy(sorted, tr, (size_t)recorded * sizeof *sorted);
    qsort(sorted, recorded, sizeof *sorted, bench_cmp_double);
    double tr_median = sorted[recorded / 2];
    double tr_p99 = sorted[(uint32_t)((double)(recorded - 1) * 0.99)];

    // Rasterization statistics: the GP0 primitive-dispatch time, a subset of the
    // budget, that a GPU worker thread would run instead.
    double rs_sum = 0.0, rs_worst = 0.0;
    for (uint32_t i = 0; i < recorded; i++) {
        rs_sum += raster[i];
        if (raster[i] > rs_worst) {
            rs_worst = raster[i];
        }
    }
    double rs_mean = rs_sum / recorded;
    memcpy(sorted, raster, (size_t)recorded * sizeof *sorted);
    qsort(sorted, recorded, sizeof *sorted, bench_cmp_double);
    double rs_median = sorted[recorded / 2];
    double rs_p99 = sorted[(uint32_t)((double)(recorded - 1) * 0.99)];

    printf("\n==== JonBoy benchmark ====\n");
    printf("workload: %s%s%s\n",
           g_bios_path ? g_bios_path : "(no bios)",
           g_cd_path ? " + " : "", g_cd_path ? g_cd_path : "");
    printf("measured %u frames (after %u warmup)\n", recorded, warmup_frames);
    printf("\nemulation-thread wall time per frame (budget %.2f ms):\n", budget);
    printf("  mean    %7.2f ms   (%.2fx real-time, %.1f fps sustained)\n",
           mean, realtime, sustained_fps);
    printf("  median  %7.2f ms\n", median);
    printf("  p99     %7.2f ms\n", p99);
    printf("  max     %7.2f ms\n", worst);
    printf("  over budget: %u / %u frames (%.1f%%)\n",
           over, recorded, 100.0 * over / recorded);
    printf("  (includes the end-of-frame VRAM publish copy)\n");
    printf("  of which rasterization (GP0 dispatch, movable to a GPU worker):\n");
    printf("    mean %.2f ms (%.0f%% of budget)   median %.2f ms   p99 %.2f ms   max %.2f ms\n",
           rs_mean, mean > 0.0 ? 100.0 * rs_mean / mean : 0.0,
           rs_median, rs_p99, rs_worst);
    printf("\naudio model (%.0f ms SPU ring @ %d Hz):\n", ring_cap, AUDIO_RATE);
    printf("  underrun events: %u   starved frames: %u   total gap: %.1f ms\n",
           underrun_events, starved, gap_ms);
    printf("\nvideo model (60 Hz present):\n");
    printf("  sustained %.1f fps   dropped %.0f frames over %.2f s wall\n",
           sustained_fps, dropped, total_ms / 1000.0);
    printf("\npresent overhead (main thread, not in the budget above):\n");
    printf("  transpose: mean %.2f ms   median %.2f ms   p99 %.2f ms   max %.2f ms\n",
           tr_mean, tr_median, tr_p99, tr_worst);
    printf("\nnote: budget = core (CPU+GPU raster+SPU+DMA+CD) plus the emu-thread\n"
           "      publish copy. The transpose runs on the vsync-limited main\n"
           "      thread in parallel, so it is shown apart. The SDL texture upload\n"
           "      and present themselves need a window and are not measured here.\n");

    free(sorted);
    free(ms);
    free(tr);
    free(raster);
    free(bench_pub);
    free(bench_row);
    return 0;
}

// Per-step service for the gdb run loop: keeps the loader and sideload boot live
// under gdb, the same way the other run loops call service_loader_boot.
static int gdb_kernel_up;
static int gdb_sideloaded;

static void
gdb_step_service(void)
{
    service_loader_boot(&gdb_kernel_up, &gdb_sideloaded);
}

// Boot the BIOS under gdb control: load the devices, reset to the boot vector,
// then hand execution to the gdb remote protocol server. The CPU starts halted
// at the reset vector, so gdb can set breakpoints and continue. Runs headless.
static int
run_gdb(int port)
{
    if (mem_load_bios(g_bios_path) == 0) {
        printf("BIOS loaded.\n");
    } else {
        printf("No BIOS loaded, continuing with blank ROM.\n");
    }

    if (g_psio_firmware) {
        load_psio_firmware(g_psio_firmware);
    }
    if (g_psio_sd) {
        load_virtual_sd_card(g_psio_sd);
    }
    if (g_psio_boot) {
        inject_boot_stub(bios_rom);
        printf("PSIO boot stub injected.\n");
    }
    if (g_games_dir) {
        loader_set_games_dir(g_games_dir);
    }
    if (g_cd_path) {
        if (cdrom_mount(g_cd_path) == 0) {
            printf("Mounted disc %s\n", g_cd_path);
        } else {
            fprintf(stderr, "cd: failed to mount %s\n", g_cd_path);
        }
    }
    if (g_memcard_path && mc_mount_slot(0, g_memcard_path) == 0) {
        printf("Memory card (slot 1) %s\n", g_memcard_path);
    }
    if (g_memcard2_path && mc_mount_slot(1, g_memcard2_path) == 0) {
        printf("Memory card (slot 2) %s\n", g_memcard2_path);
    }

    cpu_reset(0xBFC00000);
    // Let BREAK vector to the handler so real code under debug runs normally;
    // gdb breakpoints are tracked separately by address, not by patching BREAK.
    cpu.halt_on_break = 0;

    if (gdb_start(port) != 0) {
        return 1;
    }
    gdb_run(gdb_step_service);
    gdb_stop();
    printf("gdb: session ended.\n");
    return 0;
}

static void
print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s                              run the SDL windowed emulator\n"
            "  %s --headless <file> [opts]     run a flat binary with no window\n"
            "  %s --exe <file.exe> [opts]      run a PS-EXE headless (conformance)\n"
            "  %s --boot [opts]                boot the BIOS headless\n"
            "  %s --bench [opts]               measure core wall time per frame\n"
            "  %s --gdb [port]                 boot under a gdb remote server\n"
            "                                  (default port 1234, loopback only)\n"
            "  %s --savestate-selftest [opts]  prove a save/load round-trip is\n"
            "                                  deterministic (--steps sets warmup depth)\n"
            "Boot options (also apply to the windowed emulator):\n"
            "  --bios <file>       BIOS image to load (default bios.bin)\n"
            "  --psio              inject the PSIO boot stub over the BIOS entry\n"
            "  --sd <file>         mount a virtual SD-card image\n"
            "  --firmware <file>   load a PSIO firmware ROM image\n"
            "  --games <dir>       directory the loader menu browses (auto-loads\n"
            "                      the menu in the windowed build; pick with the pad)\n"
            "  --cd <file>         mount a disc image (.bin or .cue)\n"
            "  --memcard <file>    slot 1 memory card image (raw 128 KB; created if absent)\n"
            "  --memcard2 <file>   slot 2 memory card image (raw 128 KB; created if absent)\n"
            "  --script <file>     run a timed system-event script (pad, disc)\n"
            "Benchmark options (with --bench; uses the boot options above):\n"
            "  --bench-warmup <n>  frames to run before measuring (default 300)\n"
            "  --bench-frames <n>  frames to measure (default 600, ~10 s console)\n"
            "Headless options:\n"
            "  --load-addr <hex>   physical load address (default 0)\n"
            "  --pc <hex>          entry PC (default = load address)\n"
            "  --steps <n>         max instructions to run\n"
            "  --vram-out <file>   write the 320x240 VRAM region as a PPM image\n",
            prog, prog, prog, prog, prog, prog, prog);
}

// Byte-compare two files. Returns 0 if identical, -1 otherwise.
static int
files_equal(const char *pa, const char *pb)
{
    FILE *a = fopen(pa, "rb");
    FILE *b = fopen(pb, "rb");
    int eq = (a && b);
    if (a && b) {
        for (;;) {
            char ba[65536], bb[65536];
            size_t na = fread(ba, 1, sizeof ba, a);
            size_t nb = fread(bb, 1, sizeof bb, b);
            if (na != nb || memcmp(ba, bb, na) != 0) {
                eq = 0;
                break;
            }
            if (na == 0) {
                break;
            }
        }
    }
    if (a) {
        fclose(a);
    }
    if (b) {
        fclose(b);
    }
    return eq ? 0 : -1;
}

// Deterministic save-state round-trip test. It boots the BIOS (and any mounted
// disc) headless, then proves that a saved state fully and exactly captures the
// machine:
//   1. run warmup steps to reach state N, save it as A;
//   2. run span more steps to state N+M, save it as B (the reference future);
//   3. reload A, re-save as A2, and confirm A == A2 (save/load is idempotent, so
//      no mutable state is dropped on the way through a file);
//   4. from the reloaded A, run span steps again and save as C.
// If serialization is complete and load is exact, B and C are byte-identical:
// the state at N determined the next M steps with no hidden carry. A divergence
// means some live state was not captured. No SDL, no timing, fully reproducible.
static int
run_savestate_selftest(uint64_t warmup, uint64_t span)
{
    char pa[64], pb[64], pc[64], pa2[64];
    unsigned tag = (unsigned)(SDL_GetPerformanceCounter() & 0xFFFFFFu);
    SDL_snprintf(pa, sizeof pa, "/tmp/jbss_%06x_a.sav", tag);
    SDL_snprintf(pb, sizeof pb, "/tmp/jbss_%06x_b.sav", tag);
    SDL_snprintf(pc, sizeof pc, "/tmp/jbss_%06x_c.sav", tag);
    SDL_snprintf(pa2, sizeof pa2, "/tmp/jbss_%06x_a2.sav", tag);

    if (mem_load_bios(g_bios_path) != 0) {
        fprintf(stderr, "selftest: need a BIOS (--bios)\n");
        return 1;
    }
    if (g_cd_path && cdrom_mount(g_cd_path) == 0) {
        printf("selftest: mounted disc %s\n", g_cd_path);
    }
    cpu_reset(0xBFC00000);
    cpu.halt_on_break = 1;

    printf("selftest: warmup %llu steps...\n", (unsigned long long)warmup);
    for (uint64_t i = 0; i < warmup && !cpu.halted; i++) {
        step_cpu();
    }
    if (savestate_save(pa) != 0) {
        fprintf(stderr, "selftest: save A failed\n");
        return 1;
    }

    for (uint64_t i = 0; i < span && !cpu.halted; i++) {
        step_cpu();
    }
    if (savestate_save(pb) != 0) {
        fprintf(stderr, "selftest: save B failed\n");
        return 1;
    }

    if (savestate_load(pa) != 0 || savestate_save(pa2) != 0) {
        fprintf(stderr, "selftest: reload/resave A failed\n");
        return 1;
    }
    int idempotent = (files_equal(pa, pa2) == 0);

    for (uint64_t i = 0; i < span && !cpu.halted; i++) {
        step_cpu();
    }
    if (savestate_save(pc) != 0) {
        fprintf(stderr, "selftest: save C failed\n");
        return 1;
    }
    int deterministic = (files_equal(pb, pc) == 0);

    remove(pa);
    remove(pb);
    remove(pc);
    remove(pa2);

    printf("selftest: idempotent reload   %s\n", idempotent ? "PASS" : "FAIL");
    printf("selftest: deterministic replay %s (warmup=%llu span=%llu)\n",
           deterministic ? "PASS" : "FAIL",
           (unsigned long long)warmup, (unsigned long long)span);
    int ok = idempotent && deterministic;
    printf("RESULT: save-state self-test %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int
main(int argc, char *argv[])
{
    const char *headless_file = NULL;
    const char *exe_file = NULL;
    const char *vram_out = NULL;
    int boot_mode = 0;
    int selftest_mode = 0;
    int bench_mode = 0;
    uint32_t bench_warmup = 300;   // frames to run before measuring (past boot)
    uint32_t bench_frames = 600;   // frames to measure (~10 s console time)
    int gdb_mode = 0;
    int gdb_port = 1234;
    uint32_t load_addr = 0;
    uint32_t start_pc = 0;
    int have_pc = 0;
    int have_steps = 0;
    uint64_t max_steps = 1000000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0 && i + 1 < argc) {
            headless_file = argv[++i];
        } else if (strcmp(argv[i], "--exe") == 0 && i + 1 < argc) {
            exe_file = argv[++i];
        } else if (strcmp(argv[i], "--bench") == 0) {
            bench_mode = 1;
        } else if (strcmp(argv[i], "--bench-warmup") == 0 && i + 1 < argc) {
            bench_warmup = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--bench-frames") == 0 && i + 1 < argc) {
            bench_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--boot") == 0) {
            boot_mode = 1;
        } else if (strcmp(argv[i], "--gdb") == 0) {
            gdb_mode = 1;
            // An optional numeric argument overrides the default port.
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                gdb_port = (int)strtoul(argv[++i], NULL, 0);
            }
        } else if (strcmp(argv[i], "--sideload") == 0 && i + 1 < argc) {
            g_sideload_exe = argv[++i];
            boot_mode = 1; // sideload runs through the boot path
        } else if (strcmp(argv[i], "--psio") == 0) {
            g_psio_boot = 1;
        } else if (strcmp(argv[i], "--sd") == 0 && i + 1 < argc) {
            g_psio_sd = argv[++i];
        } else if (strcmp(argv[i], "--firmware") == 0 && i + 1 < argc) {
            g_psio_firmware = argv[++i];
        } else if (strcmp(argv[i], "--games") == 0 && i + 1 < argc) {
            g_games_dir = argv[++i];
        } else if (strcmp(argv[i], "--cd") == 0 && i + 1 < argc) {
            g_cd_path = argv[++i];
        } else if (strcmp(argv[i], "--memcard") == 0 && i + 1 < argc) {
            g_memcard_path = argv[++i];
        } else if (strcmp(argv[i], "--memcard2") == 0 && i + 1 < argc) {
            g_memcard2_path = argv[++i];
        } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            g_script_path = argv[++i];
        } else if (strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            g_bios_path = argv[++i];
        } else if (strcmp(argv[i], "--vram-out") == 0 && i + 1 < argc) {
            vram_out = argv[++i];
        } else if (strcmp(argv[i], "--frames-out") == 0 && i + 1 < argc) {
            g_frames_dir = argv[++i];
        } else if (strcmp(argv[i], "--frames-every") == 0 && i + 1 < argc) {
            g_frames_every = (int)strtoul(argv[++i], NULL, 0);
            if (g_frames_every < 1) g_frames_every = 1;
        } else if (strcmp(argv[i], "--frames-from") == 0 && i + 1 < argc) {
            g_frames_from = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--load-addr") == 0 && i + 1 < argc) {
            load_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--pc") == 0 && i + 1 < argc) {
            start_pc = (uint32_t)strtoul(argv[++i], NULL, 0);
            have_pc = 1;
        } else if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
            max_steps = strtoull(argv[++i], NULL, 0);
            have_steps = 1;
        } else if (strcmp(argv[i], "--savestate-selftest") == 0) {
            selftest_mode = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    // Auto-sideload the loader menu when a games directory is set but no explicit
    // program was given. This is what makes the menu usable in the windowed build
    // (where the keyboard drives the pad); it also applies to a --boot run.
    if (g_games_dir && !g_sideload_exe) {
        g_sideload_exe = LOADER_DEFAULT_PATH;
    }

    if (g_script_path && script_load(g_script_path) == 0) {
        printf("Running script %s\n", g_script_path);
    }

    if (gdb_mode) {
        return run_gdb(gdb_port);
    }

    if (bench_mode) {
        if (bench_frames == 0) {
            fprintf(stderr, "bench: --bench-frames must be at least 1\n");
            return 1;
        }
        return run_bench(bench_warmup, bench_frames);
    }

    if (selftest_mode) {
        // --steps sets the warmup depth (default 20M, well past BIOS POST into
        // GPU/DMA/timer activity); the replay span is a quarter of that.
        uint64_t warmup = have_steps ? max_steps : 20000000;
        return run_savestate_selftest(warmup, warmup / 4 + 1);
    }

    if (boot_mode) {
        return run_boot(g_psio_boot, g_psio_sd, g_psio_firmware,
                        have_steps ? max_steps : 1000, vram_out);
    }

    if (exe_file) {
        // These tests draw once then spin, so run a generous budget by default.
        return run_exe(exe_file, have_steps ? max_steps : 20000000, vram_out);
    }

    if (headless_file) {
        if (!have_pc) {
            start_pc = load_addr;
        }

        return run_headless(headless_file, load_addr, start_pc, max_steps, vram_out);
    }

    return SDL_RunApp(argc, argv, jonboy_runapp, NULL);
}
