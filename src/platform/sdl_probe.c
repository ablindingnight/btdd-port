/* Checks that SDL3 builds and links with each compiler. Opens no window. M2 replaces this with the
 * real platform layer. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    int v = SDL_GetVersion();
    (void)argc;
    (void)argv;
    printf("SDL %d.%d.%d (%s)\n", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v),
           SDL_GetRevision());
    return 0;
}
