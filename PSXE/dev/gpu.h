#ifndef GPU_H
#define GPU_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* On its own - the GPU board builds this file without the rest of the
   emulator - the interrupt controller is just a type name. */
#ifdef PSX_GPU_STANDALONE
typedef struct psx_ic_t psx_ic_t;
#else
#include "ic.h"
#endif

#define PSX_GPU_BEGIN 0x1f801810
#define PSX_GPU_SIZE 0x8
#define PSX_GPU_END 0x1f801814

#define PSX_GPU_FB_WIDTH 1024
#define PSX_GPU_FB_HEIGHT 512

/*
    VRAM rows are PSX_GPU_VRAM_PITCH halfwords apart: pixel (x, y) is
    vram[PSX_VRAM_AT(x, y)]. 1024 is the PSX's own layout. The RT1050 pads every
    row by one 32 byte cache line: at 2048 bytes a row, every fourth row falls
    into the same sets of the Cortex-M7's 32 KB 4-way data cache, so at any one
    x only 16 rows of a texture - and of the frame being drawn - can be in the
    cache together, and a texture walked down a triangle misses all the time.
    Simulated over Tekken 3 frames: 16.7% of texel fetches and 10.7% of frame
    buffer writes missed, 2.8% and 6.6% with the padded rows (three times fewer
    line fills). The padding is never drawn to nor shown.
*/
#ifndef PSX_GPU_VRAM_PITCH
#ifdef PSX_GPU_STANDALONE
#define PSX_GPU_VRAM_PITCH 1024
#else
#define PSX_GPU_VRAM_PITCH 1040
#endif
#endif

#define PSX_VRAM_AT(x, y) ((x) + ((y) * PSX_GPU_VRAM_PITCH))

// Use this when updating your texture: bytes from one VRAM row to the next
#define PSX_GPU_FB_STRIDE (PSX_GPU_VRAM_PITCH * 2)

#define PSX_GPU_VRAM_SIZE (PSX_GPU_VRAM_PITCH * PSX_GPU_FB_HEIGHT * 2)

#define PSX_GPU_CLOCK_NTSC 536932       // 53.693175 MHz
#define PSX_GPU_CLOCK_FREQ_NTSC 1.0739f // Closer to original working values
#define PSX_GPU_CLOCK_FREQ_PAL 1.0641f  // Closer to original working values

// #define PSX_GPU_CLOCK_NTSC 53.693175 // 53.693175 MHz
// #define PSX_GPU_CLOCK_FREQ_NTSC 53.693175 // 53.693175 MHz
// #define PSX_GPU_CLOCK_FREQ_PAL 53.203425 // 53.203425 MHz

enum
{
    GPU_EVENT_DMODE,
    GPU_EVENT_VBLANK,
    GPU_EVENT_VBLANK_END,
    GPU_EVENT_HBLANK,
    GPU_EVENT_HBLANK_END,
    GPU_EVENT_VBLANK_TIMER
};

enum
{
    GPU_STATE_RECV_CMD,
    GPU_STATE_RECV_ARGS,
    GPU_STATE_RECV_DATA
};

struct psx_gpu_t;

typedef struct psx_gpu_t psx_gpu_t;

typedef void (*psx_gpu_cmd_t)(psx_gpu_t *);
typedef void (*psx_gpu_event_callback_t)(psx_gpu_t *);

enum
{
    RS_VARIABLE,
    RS_1X1,
    RS_8X8,
    RS_16X16
};

enum
{
    RA_RAW = 0x01,
    RA_TRANSP = 0x02,
    RA_TEXTURED = 0x04
};

enum
{
    PA_RAW = 0x01,
    PA_TRANSP = 0x02,
    PA_TEXTURED = 0x04,
    PA_QUAD = 0x08,
    PA_SHADED = 0x10
};

typedef struct
{
    int16_t x, y;
    uint32_t c;
    uint8_t tx, ty;
} vertex_t;

typedef struct
{
    uint8_t attrib;
    vertex_t v[4];
    uint16_t clut, texp;
} poly_data_t;

typedef struct
{
    uint8_t attrib;
    vertex_t v0;
    uint16_t clut;
    uint16_t width, height;
} rect_data_t;

struct psx_gpu_t
{
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    void *udata[4];

    uint16_t *vram;
    uint16_t *empty;

    /* Set on every GPU register write, cleared when the frame is presented:
       lets the frontend skip re-scaling a frame that did not change. */
    int32_t vram_dirty;
    int32_t draw_visible; /* the drawing area overlaps the display window */

    /* Rows of VRAM the display window covers (overscan margin included), and
       the rows written on screen since the last presented frame, both as
       [y0, y1). See vram_dirty in gpu.c. */
    uint16_t vis_y0, vis_y1;
    uint16_t dirty_y0, dirty_y1;
    int display_enable;

    // State data
    uint32_t buf[16];
    uint32_t recv_data;
    int buf_index;
    int cmd_args_remaining;
    int cmd_data_remaining;
    int line_done;
    vertex_t prev_line_vertex;

    // Command counters
    uint32_t color;
    uint32_t xpos, ypos;
    uint32_t xsiz, ysiz;
    uint32_t tsiz;
    uint32_t addr;
    uint32_t xcnt, ycnt;
    vertex_t v0, v1, v2, v3;
    uint32_t pal, texp;
    uint32_t c0_xcnt, c0_ycnt;
    uint32_t c0_addr;
    int c0_xsiz, c0_ysiz;
    int c0_tsiz;
    int gp1_10h_req;

    // GPU state
    uint32_t state;

    uint32_t display_mode;
    uint32_t gpuread;
    uint32_t gpustat;

    // Drawing area
    uint32_t draw_x1, draw_y1;
    uint32_t draw_x2, draw_y2;

    // Drawing offset
    int32_t off_x, off_y;

    // Texture Window
    uint32_t texw_mx, texw_my;
    uint32_t texw_ox, texw_oy;

    // CLUT offset
    uint32_t clut_x, clut_y;

    // Texture page
    uint32_t texp_x, texp_y;
    uint32_t texp_d;

    // Display area
    uint32_t disp_x, disp_y;
    uint32_t disp_x1, disp_x2;
    uint32_t disp_y1, disp_y2;

    // Timing and IRQs
    /* GPU dot clock accumulator in 16.16 fixed point: integer compares in the
       hblank check are far cheaper than the float ones this used to do on
       every single device update. */
    uint32_t cycles_fp;
    /* Next dot clock value at which an hblank edge happens, so the periodic
       update can bail out with a single compare. */
    uint32_t next_edge_fp;
    int in_hblank;
    int line;
    psx_ic_t *ic;

    psx_gpu_event_callback_t event_cb_table[8];

    /* 480 line interlaced output: the field on screen (0 or 1, counted from
       disp_y), which changes at every vertical blank, and the parity (y & 1)
       of the rows the GPU must not draw into while it is there - -1 when every
       row is drawn. See gpu_update_field in gpu.c. */
    int32_t field;
    int32_t skip_rows;

    /* The frontend shows one field of a 480 line picture only, the rows from
       disp_y on in steps of two (psx_gpu_set_one_field): what field drawing
       puts into the other field of the display window is then never seen and
       is left out. See gpu_update_hidden in gpu.c. */
    int32_t one_field;
    int32_t field_reads;  /* the game read the window back meanwhile: stop leaving out */
    int32_t field_guard;  /* fields may be left out: reads of the window are watched */
    int32_t field_hidden; /* what is drawn now goes into the field nobody sees */
    int32_t draw_in_window; /* the drawing area lies wholly inside the display window */
    int32_t skip_prims;   /* both: polygons, rectangles and lines are left out */

    /*
        The share of the drawing area this board rasterizes, when two boards
        divide the work: band_share is 0 to 256 (256 = all of it) and band_top
        says which end it takes. The split follows the drawing area rather than
        fixed rows of VRAM, so a game that draws into two buffers in turn splits
        the same way in either of them. Uploads and VRAM copies ignore it: both
        boards keep every texture, only drawing is divided.
        draw_ry1/draw_ry2 are the drawing area narrowed to this board's share -
        what the rasterizer clips to. The game reads back its own draw_y1/draw_y2.
    */
    /* an upload into a frame buffer is colour, one into the texture area is
       palette indices: only the first is converted (see gpu_upload_is_image) */
    int32_t upload_img;

    /* the two frame buffer rectangles an upload or a read back is judged
       against, pixel by pixel: [x0, x1) x [y0, y1), the display window and the
       drawing area, empty when x0 == x1 (see gpu_img_rects in gpu.c) */
    uint16_t img_rect[2][4];

    int32_t band_share, band_top;
    int32_t draw_ry1, draw_ry2;
};

psx_gpu_t *psx_gpu_create(void);
void psx_gpu_init(psx_gpu_t *, psx_ic_t *);
uint32_t psx_gpu_read32(psx_gpu_t *, uint32_t);
uint16_t psx_gpu_read16(psx_gpu_t *, uint32_t);
uint8_t psx_gpu_read8(psx_gpu_t *, uint32_t);
void psx_gpu_write32(psx_gpu_t *, uint32_t, uint32_t);
void psx_gpu_write16(psx_gpu_t *, uint32_t, uint16_t);
void psx_gpu_write8(psx_gpu_t *, uint32_t, uint8_t);
void psx_gpu_destroy(psx_gpu_t *);
void psx_gpu_set_udata(psx_gpu_t *, int, void *);
void psx_gpu_set_event_callback(psx_gpu_t *, int, psx_gpu_event_callback_t);
void *psx_gpu_get_display_buffer(psx_gpu_t *);
void psx_gpu_update(psx_gpu_t *, int);
uint32_t psx_gpu_cycles_to_edge(const psx_gpu_t *);

/* The vertical blank as another board's GPU timing saw it: the field that
   goes on screen now. What gpu_hblank_event does at the blank, without the
   interrupt and the callbacks. */
void psx_gpu_set_field(psx_gpu_t *, uint32_t field);

/* 1: the frontend shows only the field of a 480 line picture that starts at
   disp_y (see one_field in psx_gpu_t), 0: it shows every row */
void psx_gpu_set_one_field(psx_gpu_t *, int32_t on);

/* the share of the drawing area this board draws (see band_share in psx_gpu_t) */
void psx_gpu_set_band(psx_gpu_t *, int32_t share, int32_t top);

/* Copies whole rows of a CPU -> VRAM transfer straight into VRAM, bypassing the
   per word command path. Returns the 32 bit words consumed (possibly zero, in
   which case the caller has to fall back to psx_gpu_write32). */
uint32_t psx_gpu_write_bulk(psx_gpu_t *, const uint32_t *, uint32_t);

#endif
