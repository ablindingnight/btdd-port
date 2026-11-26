/* PondToPavement.exe and btdd_play.exe are GUI programs (no console window when double-clicked or started by the
 * launcher). When started from a terminal, their printf / SDL_Log output goes to that terminal; with pipes (tests,
 * check_all) the inherited handles already work and nothing changes. */
#include "console.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

void console_setup(int alloc)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != NULL && out != INVALID_HANDLE_VALUE && !alloc)
        return; /* redirected to a file or pipe: keep it */
    if (AttachConsole(ATTACH_PARENT_PROCESS) || (alloc && AllocConsole())) {
        FILE *f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
}
#else
void console_setup(int alloc) { (void)alloc; }
#endif
