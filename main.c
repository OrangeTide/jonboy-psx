#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "cpu.h"
#include "mem.h"
#include "vram.h"
#include "rom.h"

#define TARGET_FPS 15
#define CYCLES_PER_FRAME 1000 // TODO: this low value is for debug purposes

int SDLCALL jonboy_runapp(int argc, char *argv[]);

// Simple application state structure
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *vram_texture; // Target texture handle for rendering
    bool is_running;
} AppContext;

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

    // SDL_RenderTexture automatically handles stretching the 1024x512 texture
    // to whatever dimensions your actual SDL_Window currently spans.
    SDL_RenderTexture(app->renderer, app->vram_texture, NULL, NULL);
    SDL_RenderPresent(app->renderer);
}

int SDLCALL
jonboy_runapp(int argc, char *argv[])
{
    (void)argc, (void)argv;

    // 1. Initialize SDL3 subsystems
    if (!SDL_Init(SDL_INIT_VIDEO)) {
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
    // Format BGR565 accurately represents 16-bit 5-bit color channels without manual translation.
    // SDL_TEXTUREACCESS_STREAMING allows us to dynamically lock and overwrite pixels every frame.
    app.vram_texture = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_BGR565, SDL_TEXTUREACCESS_STREAMING, VRAM_WIDTH, VRAM_HEIGHT);

    if (!app.vram_texture) {
        SDL_Log("Failed to create VRAM texture: %s", SDL_GetError());
        SDL_DestroyRenderer(app.renderer);
        SDL_DestroyWindow(app.window);
        SDL_Quit();
        return 1;
    }

    // 3. Load the software ROM/BIOS image binary into your virtual memory array
    // rom_load returns 0 on success. A missing BIOS is not fatal during
    // development: the window still opens, it just shows a blank framebuffer.
    if (rom_load("bios.bin") == 0) {
        printf("ROM BIOS loaded.\n");
    } else {
        printf("No BIOS loaded, continuing with blank RAM.\n");
    }

    cpu_reset(0);

    // Timing tracking variables using SDL3's high-resolution performance counters
    uint64_t last_time = SDL_GetPerformanceCounter();
    double accumulator = 0.0;
    double time_bridge = 1.0 / TARGET_FPS; // ~0.016666 seconds per frame
    double counts_per_sec = (double)SDL_GetPerformanceFrequency();

    // 4. Core Emulation Frame Execution Loop
    while (app.is_running) {
        SDL_Event event;

        // Handle window closure events and keyboard escapes
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                app.is_running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE) {
                    app.is_running = false;
                }
            }
        }

        // High-precision clock timing step delta calculation
        uint64_t current_time = SDL_GetPerformanceCounter();
        double elapsed_seconds = (double)(current_time - last_time) / counts_per_sec;
        last_time = current_time;
        accumulator += elapsed_seconds;

        // Process a complete frame chunk if our time accumulator step threshold is passed
        while (accumulator >= time_bridge) {

            // Execute exactly 564,480 sequential MIPS CPU instructions to match 60Hz timing
            for (uint32_t cycle = 0; cycle < CYCLES_PER_FRAME; cycle++) {
                if (cpu.halted) {
                    break;
                }

                step_cpu();
                check_hardware_interrupts(); // Evaluate peripheral lines
            }

            accumulator -= time_bridge;

            // 5. Call optimized video renderer hook
            render_vram_to_screen(&app);
        }

        // Prevent your host CPU from pinning to 100% core usage between cycles
        SDL_Delay(1);
    }

    // 6. Clean Cleanup Strategy
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

static int
run_headless(const char *path, uint32_t load_addr, uint32_t start_pc, uint64_t max_steps)
{
    if (mem_load_file(path, load_addr) != 0) {
        fprintf(stderr, "headless: failed to load %s\n", path);
        return 1;
    }

    printf("Loaded %s at 0x%08X, entry PC 0x%08X, step limit %llu\n",
           path, load_addr, start_pc, (unsigned long long)max_steps);

    cpu_reset(start_pc);

    uint64_t steps = 0;

    while (!cpu.halted && steps < max_steps) {
        step_cpu();
        check_hardware_interrupts();
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

    return 0;
}

static void
print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s                              run the SDL windowed emulator\n"
            "  %s --headless <file> [opts]     run a flat binary with no window\n"
            "Headless options:\n"
            "  --load-addr <hex>   physical load address (default 0)\n"
            "  --pc <hex>          entry PC (default = load address)\n"
            "  --steps <n>         max instructions to run (default 1000000)\n",
            prog, prog);
}

int
main(int argc, char *argv[])
{
    const char *headless_file = NULL;
    uint32_t load_addr = 0;
    uint32_t start_pc = 0;
    int have_pc = 0;
    uint64_t max_steps = 1000000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0 && i + 1 < argc) {
            headless_file = argv[++i];
        } else if (strcmp(argv[i], "--load-addr") == 0 && i + 1 < argc) {
            load_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--pc") == 0 && i + 1 < argc) {
            start_pc = (uint32_t)strtoul(argv[++i], NULL, 0);
            have_pc = 1;
        } else if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
            max_steps = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (headless_file) {
        if (!have_pc) {
            start_pc = load_addr;
        }

        return run_headless(headless_file, load_addr, start_pc, max_steps);
    }

    return SDL_RunApp(argc, argv, jonboy_runapp, NULL);
}
