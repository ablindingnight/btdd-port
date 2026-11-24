/* Menu model (see menu.h). */
#include "menu.h"
#include <stdio.h>
#include <string.h>

void menu_open(Menu *m, int page)
{
    memset(m, 0, sizeof *m);
    m->open = 1;
    m->page = page;
}

void menu_close(Menu *m)
{
    m->open = 0;
}

void menu_begin(Menu *m, const char *title)
{
    m->count = 0;
    snprintf(m->title, sizeof m->title, "%s", title);
}

void menu_add(Menu *m, MenuItemKind kind, int id, const char *label, const char *value, const char *help)
{
    MenuItem *it;
    if (m->count == MENU_MAX_ITEMS)
        return;
    it = &m->items[m->count++];
    it->kind = kind;
    it->id = id;
    snprintf(it->label, sizeof it->label, "%s", label ? label : "");
    snprintf(it->value, sizeof it->value, "%s", value ? value : "");
    snprintf(it->help, sizeof it->help, "%s", help ? help : "");
}

static int selectable(const MenuItem *it)
{
    return it->kind != ITEM_INFO;
}

/* keep the cursor on a selectable item */
static void fix_cursor(Menu *m, int dir)
{
    int *c = &m->cursor[m->page % MENU_MAX_PAGES], n;
    if (!m->count)
        return;
    if (*c >= m->count)
        *c = m->count - 1;
    if (*c < 0)
        *c = 0;
    for (n = 0; n < m->count && !selectable(&m->items[*c]); n++)
        *c = (*c + (dir < 0 ? m->count - 1 : 1)) % m->count;
}

MenuEvent menu_input(Menu *m, MenuInput in, int *id, int *dir)
{
    int *c = &m->cursor[m->page % MENU_MAX_PAGES];
    MenuItem *it;
    *id = 0;
    *dir = 0;
    if (!m->open)
        return MENU_EV_NONE;
    fix_cursor(m, 1);
    if (in == MENU_BACK) {
        if (m->depth == 0) {
            m->open = 0;
            return MENU_EV_CLOSE;
        }
        m->page = m->stack[--m->depth];
        return MENU_EV_PAGE;
    }
    if (!m->count)
        return MENU_EV_NONE;
    if (in == MENU_UP || in == MENU_DOWN) {
        int step = in == MENU_UP ? -1 : 1, n;
        for (n = 0; n < m->count; n++) {
            *c = (*c + step + m->count) % m->count;
            if (selectable(&m->items[*c]))
                break;
        }
        return MENU_EV_NONE;
    }
    it = &m->items[*c];
    *id = it->id;
    switch (it->kind) {
    case ITEM_PAGE:
        if (in != MENU_ACCEPT && in != MENU_RIGHT)
            return MENU_EV_NONE;
        if (m->depth < (int)(sizeof m->stack / sizeof m->stack[0]))
            m->stack[m->depth++] = m->page;
        m->page = it->id;
        return MENU_EV_PAGE;
    case ITEM_ACTION:
        return in == MENU_ACCEPT ? MENU_EV_ACTIVATE : MENU_EV_NONE;
    case ITEM_CHOICE:
        *dir = in == MENU_LEFT ? -1 : 1;
        return MENU_EV_CHANGE;
    default:
        return MENU_EV_NONE;
    }
}

int menu_render(Menu *m)
{
    int i, n = 0, c;
    const char *help = "";
    fix_cursor(m, 1);
    c = m->cursor[m->page % MENU_MAX_PAGES];
    snprintf(m->text[n++], MENU_COLS + 1, "%s", m->title);
    snprintf(m->text[n++], MENU_COLS + 1, "%s", "");
    for (i = 0; i < m->count; i++) {
        const MenuItem *it = &m->items[i];
        char *t = m->text[n++];
        int sel = i == c && selectable(it);
        if (sel)
            help = it->help;
        if (it->kind == ITEM_INFO) {
            snprintf(t, MENU_COLS + 1, "  %s", it->label);
        } else if (it->kind == ITEM_PAGE) {
            snprintf(t, MENU_COLS + 1, "%c %-26s >", sel ? '>' : ' ', it->label);
        } else if (it->kind == ITEM_CHOICE) {
            char v[28];
            snprintf(v, sizeof v, sel ? "<%s>" : " %s ", it->value);
            snprintf(t, MENU_COLS + 1, "%c %-16.16s%12.12s", sel ? '>' : ' ', it->label, v);
        } else {
            snprintf(t, MENU_COLS + 1, "%c %-16.16s %11.11s", sel ? '>' : ' ', it->label, it->value);
        }
    }
    if (help[0]) { /* up to two lines, wrapped at a space */
        size_t len = strlen(help), cut = len;
        snprintf(m->text[n++], MENU_COLS + 1, "%s", "");
        if (len > MENU_COLS) {
            for (cut = MENU_COLS; cut > 0 && help[cut] != ' '; cut--)
                ;
            if (!cut)
                cut = MENU_COLS;
        }
        snprintf(m->text[n++], MENU_COLS + 1, "%.*s", (int)cut, help);
        if (cut < len)
            snprintf(m->text[n++], MENU_COLS + 1, "%s", help + cut + (help[cut] == ' '));
    }
    for (i = 0; i < n; i++)
        m->lines[i] = m->text[i];
    return n;
}
