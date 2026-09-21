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

#include "dev/cdrom/disc.h"

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

/*
    A multi track disc is a cue sheet and a file per track, and only the sheet is
    the game: whatever a sheet on the card names as one of its FILEs comes off
    the list. (Tekken 3 is three files; the data track alone is big enough to
    pass for an image, and an audio track would be listed and never start.)
*/
static char __attribute__((section(".bss.$BOARD_SDRAM"))) g_sheet_text[8192];

static uint32_t menu_dir_len(const char *path)
{
    uint32_t len = 0;

    for (uint32_t i = 0; path[i]; i++)
    {
        if (path[i] == '/')
            len = i + 1u;
    }

    return len;
}

/* `a`, which is a_len long, against the string b, ignoring case */
static int menu_same_text(const char *a, uint32_t a_len, const char *b)
{
    for (uint32_t i = 0; i < a_len; i++)
    {
        char ca = a[i];
        char cb = b[i];

        if ((ca >= 'A') && (ca <= 'Z'))
            ca = (char)(ca + 32);

        if ((cb >= 'A') && (cb <= 'Z'))
            cb = (char)(cb + 32);

        if (!cb || (ca != cb))
            return 0;
    }

    return b[a_len] == 0;
}

static void menu_drop_track(const char *sheet_path, const char *name, uint32_t name_len)
{
    const uint32_t dir_len = menu_dir_len(sheet_path);

    for (int32_t j = 0; j < g_count; j++)
    {
        const menu_entry_t *e = &g_entries[j];

        /* an image of any kind, in the directory of the sheet, by that name */
        if (e->kind[0] == 'C')
            continue;

        if ((menu_dir_len(e->path) != dir_len) || strncmp(e->path, sheet_path, dir_len))
            continue;

        if (!menu_same_text(name, name_len, e->path + dir_len))
            continue;

        for (int32_t k = j; k < (g_count - 1); k++)
            g_entries[k] = g_entries[k + 1];

        g_count--;
        j--;
    }
}

static void menu_drop_cue_tracks(void)
{
    /* the sheets are looked up by path, because dropping entries moves them */
    for (int32_t i = 0; i < g_count; i++)
    {
        if (g_entries[i].kind[0] != 'C')
            continue;

        char sheet_path[MENU_PATH_MAX];

        snprintf(sheet_path, sizeof(sheet_path), "%s", g_entries[i].path);

        FIL file;
        UINT got = 0;

        if (FR_OK != f_open(&file, sheet_path, FA_READ))
            continue;

        if (FR_OK != f_read(&file, g_sheet_text, sizeof(g_sheet_text) - 1u, &got))
            got = 0;

        f_close(&file);

        g_sheet_text[got] = 0;

        for (const char *p = g_sheet_text; *p;)
        {
            while ((*p == ' ') || (*p == '\t'))
                p++;

            const char *line_end = p;

            while (*line_end && (*line_end != '\n') && (*line_end != '\r'))
                line_end++;

            if (menu_same_text(p, 4, "FILE") && ((p[4] == ' ') || (p[4] == '\t')))
            {
                const char *name = p + 5;
                const char *name_end;

                while ((name < line_end) && ((*name == ' ') || (*name == '\t')))
                    name++;

                if ((name < line_end) && (*name == '"'))
                {
                    name_end = ++name;

                    while ((name_end < line_end) && (*name_end != '"'))
                        name_end++;
                }
                else
                {
                    /* without quotes the last word is the type */
                    name_end = line_end;

                    while ((name_end > name) && (name_end[-1] != ' ') && (name_end[-1] != '\t'))
                        name_end--;

                    while ((name_end > name) && ((name_end[-1] == ' ') || (name_end[-1] == '\t')))
                        name_end--;
                }

                /* a sheet may carry the path the image had where it was made */
                for (const char *q = name; q < name_end; q++)
                {
                    if ((*q == '/') || (*q == '\\'))
                        name = q + 1;
                }

                if (name_end > name)
                    menu_drop_track(sheet_path, name, (uint32_t)(name_end - name));
            }

            p = line_end;

            while ((*p == '\n') || (*p == '\r'))
                p++;
        }

        /* this sheet may be somewhere else now */
        for (int32_t k = 0; k < g_count; k++)
        {
            if (!strcmp(g_entries[k].path, sheet_path))
                i = k;
        }
    }
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

    menu_drop_cue_tracks();
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

/* A card with a few lines of text over the list, until a tap, a button or some
   seconds have passed. */
static void menu_message(const char *title, const char *text)
{
    char lines[4][80];
    int32_t count = 0;

    /* words into lines that fit the card */
    {
        const int32_t max_w = 380;
        char line[80] = "";

        for (const char *p = text; *p && (count < 4);)
        {
            char word[48];
            uint32_t n = 0;

            while (*p == ' ')
                p++;

            while (*p && (*p != ' ') && (n < (sizeof(word) - 1u)))
                word[n++] = *p++;

            word[n] = 0;

            if (!n)
                break;

            char tried[130];

            snprintf(tried, sizeof(tried), "%s%s%s", line, line[0] ? " " : "", word);

            if ((ui_text_width(&ui_font_small, tried) > max_w) && line[0])
            {
                snprintf(lines[count++], sizeof(lines[0]), "%s", line);
                snprintf(line, sizeof(line), "%s", word);
            }
            else
            {
                snprintf(line, sizeof(line), "%s", tried);
            }
        }

        if (line[0] && (count < 4))
            snprintf(lines[count++], sizeof(lines[0]), "%s", line);
    }

    const int32_t card_h = 64 + count * 20;
    const int32_t card_y = (UI_HEIGHT - card_h) / 2;

    uint32_t prev_buttons = psxe_gamepad_raw_buttons();
    int32_t was_down = 1;

    for (int32_t frame = 0; frame < 480; frame++)
    {
        ui_begin();

        ui_vgradient(0, 0, UI_WIDTH, UI_HEIGHT, COL_BG_TOP, COL_BG_BOTTOM);
        ui_round_rect(30, card_y, UI_WIDTH - 60, card_h, 14, COL_CARD_SEL);
        ui_round_rect(30, card_y, 5, card_h, 2, COL_ACCENT);
        ui_text(&ui_font_body, 52, card_y + 32, UI_WIDTH - 104, title, COL_TEXT);

        for (int32_t i = 0; i < count; i++)
            ui_text(&ui_font_small, 52, card_y + 58 + i * 20, UI_WIDTH - 104, lines[i], COL_TEXT_DIM);

        ui_present();

        int32_t tx = 0;
        int32_t ty = 0;
        const int32_t down = psxe_touch_read(&tx, &ty);

        psxe_gamepad_poll();

        const uint32_t buttons = psxe_gamepad_raw_buttons();

        /* not within the first moments: the press that chose the game is still there */
        if ((frame > 30) && ((down && !was_down) || (buttons & ~prev_buttons)))
            break;

        was_down = down;
        prev_buttons = buttons;
    }
}

/* The chosen image is looked at before the console is started with it, so that
   one that cannot run says why instead of ending in the BIOS shell. */
static int32_t menu_start(int32_t selected, int32_t scroll, char *path, uint32_t path_size)
{
    for (int32_t i = 0; i < 24; i++)
        menu_draw(selected, scroll, -1, 1);

    if (psx_disc_probe(g_entries[selected].path) == CDT_ERROR)
    {
        PRINTF("menu: %s cannot be started: %s\r\n", g_entries[selected].path, psx_disc_last_error());

        menu_message("This one cannot be started", psx_disc_last_error());

        return 0;
    }

    snprintf(path, path_size, "%s", g_entries[selected].path);

    PRINTF("menu: starting %s\r\n", g_entries[selected].path);

    return 1;
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

#if PSXE_AUTOTEST
    for (int32_t i = 0; i < g_count; i++)
    {
        char lower[96];
        uint32_t k = 0;

        for (; g_entries[i].name[k] && (k < (sizeof(lower) - 1u)); k++)
        {
            const char ch = g_entries[i].name[k];

            lower[k] = ((ch >= 'A') && (ch <= 'Z')) ? (char)(ch + 32) : ch;
        }

        lower[k] = 0;

        if (strstr(lower, PSXE_AUTOTEST_GAME))
        {
            snprintf(path, path_size, "%s", g_entries[i].path);

            PRINTF("menu: autotest, starting %s\r\n", g_entries[i].path);

            return 1;
        }
    }
#endif

    /* nothing to choose from: straight in, unless it turns out not to be a game */
    if ((g_count == 1) && menu_start(0, 0, path, path_size))
        return 1;

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

                if (menu_start(selected, scroll, path, path_size))
                    return 1;

                warmup = 20;
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
            if (menu_start(selected, scroll, path, path_size))
                return 1;

            warmup = 20;
        }

        menu_draw(selected, scroll, press_index, 0);
    }
}
