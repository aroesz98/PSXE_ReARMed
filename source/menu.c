/*
    Game picker.

    Scans the card for disc images and shows them as a touch list on the panel;
    the controller works too, so the thing can be driven without reaching for the
    screen. Runs before the emulator is created, straight after the card is
    mounted, and draws into the LCD frame buffer through ui_draw.

    Everything here is plain polling at panel refresh rate - no interrupts, no
    timers - so it cannot interfere with the emulator that starts afterwards.
*/

#include "menu.h"

#include <string.h>
#include <stdio.h>

#include "ff.h"
#include "fsl_debug_console.h"

#include "ui_draw.h"
#include "touch.h"
#include "gamepad.h"

#define MENU_MAX_GAMES 64

#define MENU_NAME_MAX 72
#define MENU_PATH_MAX 168

/* palette */
#define COL_BG_TOP UI_RGB(0x0e, 0x11, 0x17)
#define COL_BG_BOTTOM UI_RGB(0x18, 0x1e, 0x2b)
#define COL_HEADER UI_RGB(0x12, 0x16, 0x1f)
#define COL_ACCENT UI_RGB(0x4c, 0x8d, 0xff)
#define COL_CARD UI_RGB(0x1c, 0x22, 0x30)
#define COL_CARD_SEL UI_RGB(0x26, 0x33, 0x49)
#define COL_TEXT UI_RGB(0xf2, 0xf5, 0xfa)
#define COL_TEXT_DIM UI_RGB(0x7c, 0x86, 0x98)
#define COL_TRACK UI_RGB(0x23, 0x2b, 0x3a)

/* layout */
#define HEADER_H 52
#define FOOTER_H 26
#define LIST_TOP HEADER_H
#define LIST_BOTTOM (UI_HEIGHT - FOOTER_H)
#define LIST_H (LIST_BOTTOM - LIST_TOP)
#define CARD_X 18
#define CARD_W 436
#define CARD_H 54
#define CARD_GAP 8
#define CARD_STRIDE (CARD_H + CARD_GAP)

typedef struct
{
    char name[MENU_NAME_MAX];
    char path[MENU_PATH_MAX];
    uint32_t size_mb;
    const char *kind;
} menu_entry_t;

/* a few kB, kept out of the task stack */
static menu_entry_t __attribute__((section(".bss.$BOARD_SDRAM"))) g_entries[MENU_MAX_GAMES];
static int32_t g_count;

/* ------------------------------------------------------------------ scanning */

static int menu_ext_is(const char *name, const char *ext)
{
    const uint32_t nlen = (uint32_t)strlen(name);
    const uint32_t elen = (uint32_t)strlen(ext);

    if (nlen <= elen)
        return 0;

    const char *tail = name + nlen - elen;

    for (uint32_t i = 0; i < elen; i++)
    {
        char a = tail[i];
        char b = ext[i];

        if ((a >= 'A') && (a <= 'Z'))
            a = (char)(a + 32);

        if ((b >= 'A') && (b <= 'Z'))
            b = (char)(b + 32);

        if (a != b)
            return 0;
    }

    return 1;
}

/* the image kinds worth listing, in the order they are preferred */
static const char *menu_kind_of(const char *name)
{
    if (menu_ext_is(name, ".cue"))
        return "CUE";

    if (menu_ext_is(name, ".bin"))
        return "BIN";

    if (menu_ext_is(name, ".iso"))
        return "ISO";

    if (menu_ext_is(name, ".img"))
        return "IMG";

    return 0;
}

static void menu_strip_ext(char *name)
{
    char *dot = 0;

    for (char *p = name; *p; p++)
    {
        if (*p == '.')
            dot = p;
    }

    if (dot)
        *dot = 0;
}

static int menu_same_name(const char *a, const char *b)
{
    for (;; a++, b++)
    {
        char ca = *a;
        char cb = *b;

        if ((ca >= 'A') && (ca <= 'Z'))
            ca = (char)(ca + 32);

        if ((cb >= 'A') && (cb <= 'Z'))
            cb = (char)(cb + 32);

        if (ca != cb)
            return 0;

        if (!ca)
            return 1;
    }
}

/* A PSX disc image is tens of megabytes; anything smaller that happens to end
   in .bin or .img is something else entirely - a kernel module, a BIOS dump, a
   save. A .cue is a text file and is trusted on its own. */
#define MENU_MIN_IMAGE_BYTES (8u * 1024u * 1024u)

static void menu_add(const char *dir, const char *file, uint32_t bytes)
{
    const char *kind = menu_kind_of(file);

    if (!kind || (g_count >= MENU_MAX_GAMES))
        return;

    if ((kind[0] != 'C') && (bytes < MENU_MIN_IMAGE_BYTES))
        return;

    menu_entry_t *e = &g_entries[g_count];

    /* "/" + dir + "/" + file, with the root spelled as just "/" */
    if (dir[0] && strcmp(dir, "/") != 0)
        snprintf(e->path, sizeof(e->path), "%s/%s", dir, file);
    else
        snprintf(e->path, sizeof(e->path), "/%s", file);

    snprintf(e->name, sizeof(e->name), "%s", file);
    menu_strip_ext(e->name);

    e->size_mb = bytes / (1024u * 1024u);
    e->kind = kind;

    /*
        A .cue and its .bin are the same game: keep the cue, which is what the
        CD code wants, and drop a plain track file of the same name.
    */
    for (int32_t i = 0; i < g_count; i++)
    {
        if (!menu_same_name(g_entries[i].name, e->name))
            continue;

        const int keep_new = (kind[0] == 'C');

        if (keep_new)
        {
            g_entries[i] = *e;
        }

        return;
    }

    g_count++;
}

static void menu_scan_dir(const char *path)
{
    DIR dir;

    if (FR_OK != f_opendir(&dir, path))
        return;

    for (;;)
    {
        FILINFO info;

        if ((FR_OK != f_readdir(&dir, &info)) || (info.fname[0] == 0))
            break;

        if (info.fattrib & (AM_DIR | AM_SYS | AM_HID))
            continue;

        menu_add(path, info.fname, (uint32_t)info.fsize);
    }

    f_closedir(&dir);
}

static void menu_sort(void)
{
    for (int32_t i = 1; i < g_count; i++)
    {
        const menu_entry_t key = g_entries[i];
        int32_t j = i - 1;

        while ((j >= 0) && !menu_same_name(g_entries[j].name, key.name))
        {
            /* case insensitive "greater than" */
            const char *a = g_entries[j].name;
            const char *b = key.name;
            int greater = 0;

            for (;; a++, b++)
            {
                char ca = *a;
                char cb = *b;

                if ((ca >= 'A') && (ca <= 'Z'))
                    ca = (char)(ca + 32);

                if ((cb >= 'A') && (cb <= 'Z'))
                    cb = (char)(cb + 32);

                if (ca != cb)
                {
                    greater = (ca > cb);
                    break;
                }

                if (!ca)
                    break;
            }

            if (!greater)
                break;

            g_entries[j + 1] = g_entries[j];
            j--;
        }

        g_entries[j + 1] = key;
    }
}

static void menu_scan(void)
{
    g_count = 0;

    menu_scan_dir("/");
    menu_scan_dir("/games");
    menu_scan_dir("/GAMES");
    menu_scan_dir("/psx");
    menu_scan_dir("/PSX");

    menu_sort();

    PRINTF("menu: %d disc image(s) on the card\r\n", (int)g_count);
}

/* ------------------------------------------------------------------ drawing */

static void menu_draw(int32_t selected, int32_t scroll, int32_t pressed_index, int32_t launching)
{
    char line[96];

    ui_begin();

    ui_vgradient(0, 0, UI_WIDTH, UI_HEIGHT, COL_BG_TOP, COL_BG_BOTTOM);

    /* header */
    ui_fill(0, 0, UI_WIDTH, HEADER_H, COL_HEADER);
    ui_fill(0, HEADER_H - 1, UI_WIDTH, 1, COL_TRACK);
    ui_fill(18, 16, 4, 22, COL_ACCENT);
    ui_text(&ui_font_title, 32, 37, 0, "PSXE", COL_TEXT);

    snprintf(line, sizeof(line), "%d %s", (int)g_count, (g_count == 1) ? "game" : "games");
    ui_text(&ui_font_small, UI_WIDTH - 18 - ui_text_width(&ui_font_small, line), 34, 0, line, COL_TEXT_DIM);

    /* list */
    for (int32_t i = 0; i < g_count; i++)
    {
        const int32_t y = LIST_TOP + 6 + i * CARD_STRIDE - scroll;

        if (((y + CARD_H) <= LIST_TOP) || (y >= LIST_BOTTOM))
            continue;

        const int is_sel = (i == selected);
        const int is_press = (i == pressed_index);

        ui_round_rect(CARD_X, y, CARD_W, CARD_H, 12, is_press ? COL_CARD_SEL : COL_CARD);

        if (is_sel)
        {
            ui_round_rect(CARD_X, y, 5, CARD_H, 2, COL_ACCENT);
            ui_round_rect_blend(CARD_X, y, CARD_W, CARD_H, 12, COL_ACCENT, 22);
        }

        ui_text(&ui_font_body, CARD_X + 18, y + 23, CARD_W - 36, g_entries[i].name, COL_TEXT);

        if (g_entries[i].size_mb)
            snprintf(line, sizeof(line), "%s  -  %u MB", g_entries[i].kind, (unsigned)g_entries[i].size_mb);
        else
            snprintf(line, sizeof(line), "%s", g_entries[i].kind);

        ui_text(&ui_font_small, CARD_X + 18, y + 42, CARD_W - 36, line, COL_TEXT_DIM);
    }

    /* scrollbar, only when there is something to scroll */
    const int32_t content = g_count * CARD_STRIDE + 12;

    if (content > LIST_H)
    {
        const int32_t track_y = LIST_TOP + 6;
        const int32_t track_h = LIST_H - 12;
        const int32_t thumb_h = (track_h * LIST_H) / content;
        const int32_t thumb_y = track_y + (track_h - thumb_h) * scroll / (content - LIST_H);

        ui_round_rect(UI_WIDTH - 10, track_y, 4, track_h, 2, COL_TRACK);
        ui_round_rect(UI_WIDTH - 10, thumb_y, 4, thumb_h, 2, COL_ACCENT);
    }

    /* footer */
    ui_fill(0, LIST_BOTTOM, UI_WIDTH, FOOTER_H, COL_HEADER);
    ui_fill(0, LIST_BOTTOM, UI_WIDTH, 1, COL_TRACK);

    const char *hint = psxe_gamepad_is_connected() ? "Tap a game, or use the D-pad and Cross"
                                                   : "Tap a game to start";

    ui_text(&ui_font_small, 18, LIST_BOTTOM + 18, UI_WIDTH - 36, hint, COL_TEXT_DIM);

    if (launching)
    {
        ui_fill_blend(0, 0, UI_WIDTH, UI_HEIGHT, UI_RGB(0, 0, 0), 150);

        snprintf(line, sizeof(line), "Loading %s", g_entries[selected].name);

        const int32_t w = ui_text_width(&ui_font_body, line);

        ui_round_rect((UI_WIDTH - (w + 48)) / 2, 118, w + 48, 44, 14, COL_CARD_SEL);
        ui_text(&ui_font_body, (UI_WIDTH - w) / 2, 146, 0, line, COL_TEXT);
    }

    ui_present();
}

/* ------------------------------------------------------------------ picking */

static int32_t menu_clamp_scroll(int32_t scroll)
{
    const int32_t content = g_count * CARD_STRIDE + 12;
    const int32_t max = (content > LIST_H) ? (content - LIST_H) : 0;

    if (scroll < 0)
        return 0;

    if (scroll > max)
        return max;

    return scroll;
}

/* keeps the selected card fully visible */
static int32_t menu_scroll_to(int32_t selected, int32_t scroll)
{
    const int32_t top = 6 + selected * CARD_STRIDE;
    const int32_t bottom = top + CARD_H;

    if (top < scroll)
        scroll = top;
    else if (bottom > (scroll + LIST_H))
        scroll = bottom - LIST_H;

    return menu_clamp_scroll(scroll);
}

static int32_t menu_index_at(int32_t y, int32_t scroll)
{
    if ((y < LIST_TOP) || (y >= LIST_BOTTOM))
        return -1;

    const int32_t local = y - LIST_TOP - 6 + scroll;

    if (local < 0)
        return -1;

    const int32_t index = local / CARD_STRIDE;

    /* inside the card, not in the gap below it */
    if ((local - index * CARD_STRIDE) > CARD_H)
        return -1;

    return (index < g_count) ? index : -1;
}

int32_t psxe_menu_pick(char *path, uint32_t path_size)
{
    menu_scan();

    if (g_count <= 0)
        return 0;

    const int32_t touch_ok = psxe_touch_init();


    int32_t selected = 0;
    int32_t scroll = 0;

    /*
        Whatever is already held when the picker opens must not count as a press:
        a finger still on the panel, or a button held from before, used to start
        a game immediately.
    */
    int32_t was_down = 1;
    int32_t press_y = 0;
    int32_t press_scroll = 0;
    int32_t press_index = -1;
    int32_t dragged = 0;
    int32_t held = 0;

    /* controller edge detection, seeded with what is held right now */
    uint32_t prev_buttons = psxe_gamepad_raw_buttons();

    /* and a moment of grace for the first frames after the link settles */
    int32_t warmup = 20;

    if (g_count == 1)
    {
        /* nothing to choose from, but show what is loading for a moment */
        for (int32_t i = 0; i < 30; i++)
            menu_draw(0, 0, -1, 1);

        snprintf(path, path_size, "%s", g_entries[0].path);

        PRINTF("menu: only one image, starting %s\r\n", g_entries[0].path);

        return 1;
    }

    for (;;)
    {
        int32_t tx = 0;
        int32_t ty = 0;
        const int32_t down = touch_ok ? psxe_touch_read(&tx, &ty) : 0;

        if (warmup > 0)
        {
            warmup--;

            psxe_gamepad_poll();

            prev_buttons = psxe_gamepad_raw_buttons();
            was_down = down ? 1 : 0;

            menu_draw(selected, scroll, -1, 0);

            continue;
        }

        if (down && !was_down)
        {
            press_y = ty;
            press_scroll = scroll;
            press_index = menu_index_at(ty, scroll);
            dragged = 0;
            held = 1;
        }
        else if (down && was_down)
        {
            held++;
        }
        if (down && was_down)
        {
            const int32_t delta = press_y - ty;

            if ((delta > 6) || (delta < -6))
            {
                dragged = 1;
                press_index = -1;
            }

            scroll = menu_clamp_scroll(press_scroll + delta);
        }
        else if (!down && was_down)
        {
            /* a tap that did not turn into a drag starts the game; a contact
               seen for a single poll is noise, not a tap */
            if (!dragged && (press_index >= 0) && (held >= 2))
            {
                selected = press_index;

                for (int32_t i = 0; i < 24; i++)
                    menu_draw(selected, scroll, -1, 1);

                snprintf(path, path_size, "%s", g_entries[selected].path);

                PRINTF("menu: starting %s\r\n", g_entries[selected].path);

                return 1;
            }

            press_index = -1;
        }

        was_down = down;

        /* controller */
        psxe_gamepad_poll();

        const uint32_t buttons = psxe_gamepad_raw_buttons();
        const uint32_t pressed = buttons & ~prev_buttons;

        prev_buttons = buttons;

        if (pressed & (1u << PSXE_DS_DPAD_DOWN))
        {
            if (selected < (g_count - 1))
                selected++;

            scroll = menu_scroll_to(selected, scroll);
        }

        if (pressed & (1u << PSXE_DS_DPAD_UP))
        {
            if (selected > 0)
                selected--;

            scroll = menu_scroll_to(selected, scroll);
        }

        if (pressed & ((1u << PSXE_DS_CROSS) | (1u << PSXE_DS_OPTIONS)))
        {
            for (int32_t i = 0; i < 24; i++)
                menu_draw(selected, scroll, -1, 1);

            snprintf(path, path_size, "%s", g_entries[selected].path);

            PRINTF("menu: starting %s\r\n", g_entries[selected].path);

            return 1;
        }

        menu_draw(selected, scroll, press_index, 0);
    }
}
