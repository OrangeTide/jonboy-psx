#include <stdio.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "rom.h"

int SDLCALL jonboy_runapp(int argc, char *argv[]);

// Simple application state structure
typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    bool is_running;
} AppContext;

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
    // VRAM canvas maps well into 1024x512 resolution space
    if (!SDL_CreateWindowAndRenderer("JonBoy Toy Console (R3051)", 1024, 512, 0, &app.window, &app.renderer)) {
        SDL_Log("Window/Renderer Creation Failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // 3. Load the software ROM/BIOS image binary into your virtual memory array
    int result = rom_load("bios.bin");
    printf("ROM BIOS load status result=%d\n", result);
    if (result != 0) {
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

            // 5. Render/Present Frame Buffer Component Step
            // This is where you would convert your uint16_t vram.canvas array into an
            // SDL_Texture to draw onto the screen using SDL_RenderTexture()
            SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
            SDL_RenderClear(app.renderer);
            SDL_RenderPresent(app.renderer);
        }

        // Prevent your host CPU from pinning to 100% core usage between cycles
        SDL_Delay(1);
    }

    // 6. Clean Cleanup Strategy
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
