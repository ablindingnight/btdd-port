/* PondToPavement.exe and btdd_play.exe are GUI programs (no console window when double-clicked or started by the
 * launcher). When started from a terminal, their printf / SDL_Log output goes to that terminal; with pipes (tests,
 * check_all) the inherited handles already work and nothing changes. */
#include "console.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static int has_output;

void console_setup(int alloc)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    if (((out != NULL && out != INVALID_HANDLE_VALUE) || (err != NULL && err != INVALID_HANDLE_VALUE)) && !alloc) {
        has_output = 1; /* redirected to a file or pipe (tests, the launcher's run log): keep it */
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS) || (alloc && AllocConsole())) {
        FILE *f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        has_output = 1;
    }
}

int console_has_output(void)
{
    return has_output;
}
#else
void console_setup(int alloc) { (void)alloc; }
int console_has_output(void) { return 1; }
#endif
