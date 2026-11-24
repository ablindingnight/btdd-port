/* A small text menu model for btdd_play's settings overlay (and later the launcher): pages of items, a cursor per
 * page, a page stack, input actions, and rendering to text lines. No SDL: the caller maps keys / gamepad buttons to
 * MenuInput, builds the items of the current page every frame from its own state (menu_begin + menu_add), and
 * applies MENU_EV_CHANGE / MENU_EV_ACTIVATE events.
 *
 * Item kinds: a PAGE item opens page `id`; an ACTION runs on accept; a CHOICE changes with left/right (or accept =
 * next value); INFO is a non-selectable line. */
#ifndef BTDD_MENU_H
#define BTDD_MENU_H

typedef enum MenuInput { MENU_UP, MENU_DOWN, MENU_LEFT, MENU_RIGHT, MENU_ACCEPT, MENU_BACK } MenuInput;
typedef enum MenuItemKind { ITEM_PAGE, ITEM_ACTION, ITEM_CHOICE, ITEM_INFO } MenuItemKind;
typedef enum MenuEvent { MENU_EV_NONE, MENU_EV_ACTIVATE, MENU_EV_CHANGE, MENU_EV_PAGE, MENU_EV_CLOSE } MenuEvent;

#define MENU_MAX_ITEMS 22
#define MENU_MAX_PAGES 24
#define MENU_COLS 31 /* text columns that fit 256 pixels with the 8-pixel font */

typedef struct MenuItem {
    MenuItemKind kind;
    int id;          /* page number (PAGE) or the caller's item id */
    char label[32];
    char value[24];  /* shown right-aligned (CHOICE: the current value) */
    char help[64];   /* shown at the bottom while selected */
} MenuItem;

typedef struct Menu {
    int open;
    int page;
    int cursor[MENU_MAX_PAGES];
    int stack[8], depth;
    char title[32];
    MenuItem items[MENU_MAX_ITEMS];
    int count;
    char text[MENU_MAX_ITEMS + 4][MENU_COLS + 1];
    const char *lines[MENU_MAX_ITEMS + 4];
} Menu;

void menu_open(Menu *m, int page);
void menu_close(Menu *m);
/* Rebuild the current page: call menu_begin, then menu_add for each item. */
void menu_begin(Menu *m, const char *title);
void menu_add(Menu *m, MenuItemKind kind, int id, const char *label, const char *value, const char *help);
/* Applies one input. On MENU_EV_ACTIVATE / MENU_EV_CHANGE, *id is the item id and *dir is +1/-1 (CHANGE) or 0. */
MenuEvent menu_input(Menu *m, MenuInput in, int *id, int *dir);
/* Text lines for the overlay; returns how many. */
int menu_render(Menu *m);

#endif
