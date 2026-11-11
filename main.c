#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// Map a host key to a digital pad button bit, or 0 if it is not a pad key.
static uint16_t
pad_bit_for_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_UP:        return PAD_BTN_UP;
    case SDLK_DOWN:      return PAD_BTN_DOWN;
    case SDLK_LEFT:      return PAD_BTN_LEFT;
    case SDLK_RIGHT:     return PAD_BTN_RIGHT;
    case SDLK_Z:         return PAD_BTN_CROSS;
    case SDLK_X:         return PAD_BTN_CIRCLE;
    case SDLK_A:         return PAD_BTN_SQUARE;
    case SDLK_S:         return PAD_BTN_TRIANGLE;
    case SDLK_RETURN:    return PAD_BTN_START;
    case SDLK_RSHIFT:    return PAD_BTN_SELECT;
    case SDLK_Q:         return PAD_BTN_L1;
    case SDLK_W:         return PAD_BTN_R1;
    case SDLK_E:         return PAD_BTN_L2;
    case SDLK_R:         return PAD_BTN_R2;
    case SDLK_C:         return PAD_BTN_L3;
    case SDLK_V:         return PAD_BTN_R3;
    default:             return 0;
    }
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

// SPU output is 44.1 kHz stereo 16-bit. The audio device pulls on demand through
// this callback, which runs on SDL's own audio thread and drains the SPU ring.
// So audio is produced by the emu thread and consumed here, decoupled from the
// main thread entirely: a blocked event pump or slow present cannot starve it.
#define AUDIO_RATE 44100

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

static void
publish_frame(AppContext *app)
{
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

// Main thread: take the latest published buffer (if any) and present it. When no
// new frame has been published since last time, it re-presents the buffer it
// already owns, so the display keeps showing the last frame.
static void
present_frame(AppContext *app)
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
            SDL_RenderPresent(app->renderer);
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

    SDL_RenderPresent(app->renderer);
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

    // The loader/sideload service only ever acts when a sideload EXE or a loader
    // games directory is configured. For a plain BIOS or disc boot neither is,
    // so skip the per-step call entirely and pay only a predicted branch. This
    // keeps two function calls per instruction off the emulation thread.
    int service_active = (g_sideload_exe != NULL) || (g_games_dir != NULL);

    uint64_t last_time = SDL_GetPerformanceCounter();
    double accumulator = 0.0;
    double time_bridge = 1.0 / TARGET_FPS;
    double counts_per_sec = (double)SDL_GetPerformanceFrequency();

    // Live real-time metric (JONBOY_PERF=1). busy_acc is the wall time spent
    // actually running instructions; comparing it to the wall time that elapsed
    // gives the fraction of the real-time budget the interpreter consumes. Below
    // 100% there is headroom; at 100% with fewer than 60 emulated frames the core
    // is saturated and audio underruns follow.
    int perf_hud = getenv("JONBOY_PERF") != NULL;
    uint64_t report_last = last_time;
    double busy_acc = 0.0;
    int frames_acc = 0;

    while (SDL_GetAtomicInt(&app->running)) {
        uint64_t now = SDL_GetPerformanceCounter();
        accumulator += (double)(now - last_time) / counts_per_sec;
        last_time = now;

        // Cap the backlog so a slow patch never spirals into endless catch-up.
        if (accumulator > 4.0 * time_bridge) {
            accumulator = 4.0 * time_bridge;
        }

        int ran = 0;
        uint64_t busy_start = SDL_GetPerformanceCounter();
        while (accumulator >= time_bridge && SDL_GetAtomicInt(&app->running)) {
            for (uint32_t step = 0; step < STEPS_PER_FRAME; step++) {
                if (cpu.halted) {
                    break;
                }
                if (service_active) {
                    service_loader_boot(&kernel_up, &sideloaded);
                }
                step_cpu();
            }
            accumulator -= time_bridge;
            frames_acc++;
            ran = 1;
        }

        // Publish once per outer pass, showing the latest state rather than every
        // caught-up frame that would never be seen. Publishing is counted in the
        // busy time below so the real-time budget figure reflects every piece of
        // work the emulation thread does, not just instruction execution.
        if (ran) {
            publish_frame(app);
            busy_acc += (double)(SDL_GetPerformanceCounter() - busy_start) / counts_per_sec;
        }

        if (perf_hud) {
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

    // 2. Create an SDL3 window and hardware-accelerated renderer
    if (!SDL_CreateWindowAndRenderer("JonBoy Toy Console (R3051)", 1024, 512, 0, &app.window, &app.renderer)) {
        SDL_Log("Window/Renderer Creation Failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
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

    // Open the audio output stream for SPU/CD sound. A failure here is not
    // fatal: the emulator runs silently.
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, AUDIO_RATE };
    app.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, audio_callback, NULL);
    if (app.audio) {
        SDL_ResumeAudioStreamDevice(app.audio);
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
                if (event.key.key == SDLK_ESCAPE) {
                    SDL_SetAtomicInt(&app.running, 0);
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

                uint16_t bit = pad_bit_for_key(event.key.key);
                if (bit) {
                    kbd_buttons &= ~bit; // active low: pressed clears the bit
                }
            } else if (event.type == SDL_EVENT_KEY_UP) {
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
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                       event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
                pad_toggle_analog(); // gamepad ANALOG/home button
                printf("Pad analog mode %s\n",
                       pad_analog_mode() ? "on" : "off");
            }
        }

        // Fold this frame's keyboard and gamepad state into the pad, unless a
        // script is driving the controller (it sets the pad state at VBLANK).
        // The emu thread reads these globals; the values are single words, so a
        // torn read is harmless (at worst one frame of mixed input).
        if (!script_active()) {
            update_pad_input(app.gamepad, kbd_buttons);
        }

        // Present the latest published frame. VSync paces this loop; SDL_Delay is
        // a fallback when vsync is unavailable (a dummy video driver).
        present_frame(&app);
        SDL_Delay(1);
    }

    // 6. Clean Cleanup Strategy: stop the emu thread before tearing anything down.
    SDL_SetAtomicInt(&app.running, 0);
    if (app.emu) {
        SDL_WaitThread(app.emu, NULL);
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
    uint16_t *bench_pub = malloc((size_t)VRAM_WIDTH * VRAM_HEIGHT * sizeof(uint16_t));
    uint16_t *bench_row = malloc((size_t)VRAM_WIDTH * VRAM_HEIGHT * sizeof(uint16_t));
    if (!ms || !tr || !bench_pub || !bench_row) {
        fprintf(stderr, "bench: out of memory\n");
        free(ms);
        free(tr);
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
    uint32_t recorded = 0;
    uint32_t last_frame = psx_frame_count();
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
                recorded++;
            }
            // Reset after the publish/transpose so their time is not charged to
            // the next frame's step measurement.
            t_prev = SDL_GetPerformanceCounter();
            last_frame = f;
        }
    }

    if (recorded == 0) {
        fprintf(stderr, "bench: no frames emulated (did the CPU halt during "
                "warmup?)\n");
        free(ms);
        free(tr);
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
            prog, prog, prog, prog, prog, prog);
}

int
main(int argc, char *argv[])
{
    const char *headless_file = NULL;
    const char *exe_file = NULL;
    const char *vram_out = NULL;
    int boot_mode = 0;
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
