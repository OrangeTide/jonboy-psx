#include <stdio.h>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "cpu.h"
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
    int result = rom_load("bios.bin");
    printf("ROM BIOS load status result=%d\n", result);

    if (result != 0) {
        SDL_DestroyTexture(app.vram_texture);
        SDL_DestroyRenderer(app.renderer);
        SDL_DestroyWindow(app.window);
        SDL_Quit();
        return 1;
    }

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

int
main(int argc, char *argv[])
{
    return SDL_RunApp(argc, argv, jonboy_runapp, NULL);
}
