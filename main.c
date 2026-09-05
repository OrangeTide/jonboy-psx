#include <stdio.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "cart.h"

int SDLCALL jonboy_runapp(int argc, char *argv[]);

int
main(int argc, char *argv[])
{
    return SDL_RunApp(argc, argv, jonboy_runapp, NULL);
}


int SDLCALL
jonboy_runapp(int argc, char *argv[])
{
    (void)argc, (void)argv;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return 1;
    }

    printf("Hello World\n");
    int result = cart_load("cart.bin");
    printf("result=%d\n", result);

    SDL_Quit();

    return 0;
}
