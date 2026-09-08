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
#include "sched.h"
#include "script.h"

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
static const char *g_memcard_path = NULL;    // memory-card image (raw 128 KB)
static const char *g_script_path = NULL;     // timed system-event script file

#define TARGET_FPS 60

// The CPU runs at ~33.87 MHz, which is 564480 cycles per 60 Hz frame. Each
// step_cpu call advances two cycles, so a frame is half that many steps.
#define STEPS_PER_FRAME (564480 / 2)

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
    SDL_AudioStream *audio;    // SPU output sink (NULL if audio failed to open)
    SDL_Gamepad *gamepad;      // open analog gamepad, or NULL (keyboard only)
    bool is_running;
    bool full_vram_view;       // F1 toggles between display area and full VRAM
} AppContext;

// SPU output is 44.1 kHz stereo 16-bit. Drain the SPU ring after each emulated
// frame and hand it to the audio device, keeping at most a short backlog so the
// sound stays near the picture.
#define AUDIO_RATE 44100
#define AUDIO_MAX_QUEUED_FRAMES (AUDIO_RATE / 5) // ~0.2 s latency cap

static void
pump_audio(AppContext *app)
{
    if (!app->audio) {
        return;
    }

    int16_t buf[1024 * 2];
    int n;
    while ((n = spu_output_pull(buf, 1024)) > 0) {
        // Drop rather than let latency grow without bound if emulation runs
        // ahead of real time.
        int queued = SDL_GetAudioStreamQueued(app->audio) / (int)(2 * sizeof(int16_t));
        if (queued < AUDIO_MAX_QUEUED_FRAMES) {
            SDL_PutAudioStreamData(app->audio, buf, n * 2 * (int)sizeof(int16_t));
        }
        if (n < 1024) {
            break;
        }
    }
}

static void
render_vram_to_screen(AppContext* app)
{
    void* pixels;
    int pitch;

    // 1. Lock the texture for active pixel write access
    // This gives us direct access to the backbuffer streaming pointer
    if (!SDL_LockTexture(app->vram_texture, NULL, &pixels, &pitch)) {
        SDL_Log("Failed to lock texture: %s", SDL_GetError());
        return;
    }

    uint16_t* dst_pixels = (uint16_t*)pixels;
    int dst_pitch_pixels = pitch / sizeof(uint16_t);

    // 2. Transpose the 2D VRAM array into the linear target texture memory
    // Optimized for CPU cache line streaming
    for (int y = 0; y < VRAM_HEIGHT; y++) {
        uint16_t* row_dest = &dst_pixels[y * dst_pitch_pixels];

        for (int x = 0; x < VRAM_WIDTH; x++) {
            // Map the layout from 2D coordinates straight to sequential scanlines
            row_dest[x] = vram.canvas[x][y];
        }
    }

    // 3. Unlock to submit the changes to the host GPU
    SDL_UnlockTexture(app->vram_texture);

    // 4. Render and upscale onto the screen canvas
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    SDL_RenderClear(app->renderer);

    if (app->full_vram_view) {
        // Debug view: show the entire 1024x512 VRAM stretched to the window.
        SDL_RenderTexture(app->renderer, app->vram_texture, NULL, NULL);
    } else if (!gpu.disp_blank) {
        // Normal view: show only the GPU's display area, as a TV would. When
        // the display is disabled the window stays cleared (black).
        SDL_FRect src = {
            (float)gpu.disp_x, (float)gpu.disp_y,
            (float)gpu.disp_w, (float)gpu.disp_h
        };
        SDL_RenderTexture(app->renderer, app->vram_texture, &src, NULL);
    }

    SDL_RenderPresent(app->renderer);
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

    AppContext app = { .is_running = true };

    // 2. Create an SDL3 window and hardware-accelerated renderer
    if (!SDL_CreateWindowAndRenderer("JonBoy Toy Console (R3051)", 1024, 512, 0, &app.window, &app.renderer)) {
        SDL_Log("Window/Renderer Creation Failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

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

    // Open the audio output stream for SPU/CD sound. A failure here is not
    // fatal: the emulator runs silently.
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, AUDIO_RATE };
    app.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, NULL, NULL);
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

    if (g_memcard_path && mc_mount(g_memcard_path) == 0) {
        printf("Memory card %s\n", g_memcard_path);
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

    // Loader boot progress, driven by service_loader_boot each step: the menu is
    // sideloaded at kernel-up and a menu selection loads or boots here.
    int kernel_up = 0;
    int sideloaded = 0;

    // Timing tracking variables using SDL3's high-resolution performance counters
    uint64_t last_time = SDL_GetPerformanceCounter();
    double accumulator = 0.0;
    double time_bridge = 1.0 / TARGET_FPS; // ~0.016666 seconds per frame
    double counts_per_sec = (double)SDL_GetPerformanceFrequency();

    // 4. Core Emulation Frame Execution Loop
    uint16_t kbd_buttons = 0xFFFF; // keyboard button mask, active low

    while (app.is_running) {
        SDL_Event event;

        // Handle window closure events and keyboard escapes
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                app.is_running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE) {
                    app.is_running = false;
                } else if (event.key.key == SDLK_F1) {
                    app.full_vram_view = !app.full_vram_view;
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
        if (!script_active()) {
            update_pad_input(app.gamepad, kbd_buttons);
        }

        // High-precision clock timing step delta calculation
        uint64_t current_time = SDL_GetPerformanceCounter();
        double elapsed_seconds = (double)(current_time - last_time) / counts_per_sec;
        last_time = current_time;
        accumulator += elapsed_seconds;

        // Cap the backlog so we never try to catch up more than a few frames.
        // Without this, if a frame's worth of CPU steps takes longer than one
        // real frame (a slower host, or the window was dragged/paused so the
        // accumulator ballooned), the catch-up loop below would run frame after
        // frame and never return to poll events, so the close button and keys
        // would stop responding. Dropping the backlog keeps input responsive.
        if (accumulator > 4.0 * time_bridge) {
            accumulator = 4.0 * time_bridge;
        }

        // Process a complete frame chunk if our time accumulator step threshold
        // is passed. Bail out immediately if a quit event arrived, so closing
        // the window does not wait for the whole backlog to drain.
        while (accumulator >= time_bridge && app.is_running) {

            // Run one frame's worth of CPU steps to match 60 Hz timing.
            for (uint32_t step = 0; step < STEPS_PER_FRAME; step++) {
                if (cpu.halted) {
                    break;
                }

                service_loader_boot(&kernel_up, &sideloaded);
                step_cpu(); // interrupts are recognized inside step_cpu
            }

            accumulator -= time_bridge;

            // Hand this frame's SPU output to the sound device.
            pump_audio(&app);

            // 5. Call optimized video renderer hook
            render_vram_to_screen(&app);
        }

        // Prevent your host CPU from pinning to 100% core usage between cycles
        SDL_Delay(1);
    }

    // 6. Clean Cleanup Strategy
    if (app.gamepad) {
        SDL_CloseGamepad(app.gamepad);
    }
    if (app.audio) {
        SDL_DestroyAudioStream(app.audio);
    }
    SDL_DestroyTexture(app.vram_texture);
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

// Dump the visible 320x240 top-left region of VRAM as a binary PPM. Setting the
// VRAMFULL environment variable dumps the entire 1024x512 VRAM instead, which is
// needed to inspect off-screen texture pages such as the shell flame at (896,0).
static void
dump_vram_ppm(const char *path)
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

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int r, g, b;
            bgr555_to_rgb(vram.canvas[x][y], &r, &g, &b);
            unsigned char rgb[3] = { (unsigned char)r, (unsigned char)g, (unsigned char)b };
            fwrite(rgb, 1, 3, f);
        }
    }

    fclose(f);
    printf("Wrote VRAM image to %s\n", path);
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

    while (!cpu.halted && steps < max_steps) {
        step_cpu();
        steps++;
    }

    printf("Ran %llu steps (PC=0x%08X).\n", (unsigned long long)steps, cpu.PC);

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

    if (g_memcard_path && mc_mount(g_memcard_path) == 0) {
        printf("Memory card %s\n", g_memcard_path);
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

    while (!cpu.halted && steps < max_steps) {
        service_loader_boot(&kernel_up, &sideloaded);
        step_cpu();
        steps++;
    }

    printf("Ran %llu steps.\n", (unsigned long long)steps);
    dump_registers();
    // Show the instructions around where execution ended, to spot poll loops.
    dump_memory((cpu.PC - 16) & ~0xFu, 8);

    if (vram_out) {
        dump_vram_ppm(vram_out);
    }

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
            "Boot options (also apply to the windowed emulator):\n"
            "  --bios <file>       BIOS image to load (default bios.bin)\n"
            "  --psio              inject the PSIO boot stub over the BIOS entry\n"
            "  --sd <file>         mount a virtual SD-card image\n"
            "  --firmware <file>   load a PSIO firmware ROM image\n"
            "  --games <dir>       directory the loader menu browses (auto-loads\n"
            "                      the menu in the windowed build; pick with the pad)\n"
            "  --cd <file>         mount a disc image (.bin or .cue)\n"
            "  --memcard <file>    memory card image (raw 128 KB; created if absent)\n"
            "  --script <file>     run a timed system-event script (pad, disc)\n"
            "Headless options:\n"
            "  --load-addr <hex>   physical load address (default 0)\n"
            "  --pc <hex>          entry PC (default = load address)\n"
            "  --steps <n>         max instructions to run\n"
            "  --vram-out <file>   write the 320x240 VRAM region as a PPM image\n",
            prog, prog, prog, prog);
}

int
main(int argc, char *argv[])
{
    const char *headless_file = NULL;
    const char *exe_file = NULL;
    const char *vram_out = NULL;
    int boot_mode = 0;
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
        } else if (strcmp(argv[i], "--boot") == 0) {
            boot_mode = 1;
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
        } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            g_script_path = argv[++i];
        } else if (strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            g_bios_path = argv[++i];
        } else if (strcmp(argv[i], "--vram-out") == 0 && i + 1 < argc) {
            vram_out = argv[++i];
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
