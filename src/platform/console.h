/* Console output for the GUI programs: attach to the parent terminal when there is one; alloc = 1 (--console) opens
 * a console window of its own (for debugging). No-op off Windows. */
#ifndef BTDD_CONSOLE_H
#define BTDD_CONSOLE_H
void console_setup(int alloc);
#endif
