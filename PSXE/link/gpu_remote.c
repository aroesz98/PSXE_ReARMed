/*
    The GPU on the other board: the GP0/GP1 stream over Ethernet.

    Framing (PSXE/link/psxe_link.h): records of 32 bit words in Ethernet frames
    of our own type. A frame is filled here word by word as the game writes the
    GPU, and goes out when it is full, at every vertical blank, and before a
    VRAM read waits for its data. The GPU board tells how much it has consumed
    (STATUS records) and this side never has more than PSXE_LINK_WINDOW bytes
    outstanding, so its receive ring cannot overflow; when it would, the
    emulation waits - that is the only back pressure there is.

    The shadow parser follows the GP0 stream just far enough to know where the
    commands begin (a record that starts at a command boundary is flagged, and
    the GPU board picks up there after losing frames), how long a CPU -> VRAM
    upload is (its rows are copied in bulk) and how many words a VRAM -> CPU
    read will send back. It also keeps the bits of GPUSTAT that GP0 changes,
    since gpu.c's own parser does not run here.

    The VRAM shadow: what the game uploads (GP0 A0h) and copies (GP0 80h) is
    kept in this board's own VRAM as well, so that a GPU board that starts over
    (reflashed, reset, cable out) gets its textures and palettes back - the whole
    megabyte is uploaded again when the link comes up. What the GPU drew is not
    shadowed: the game draws the next frame anyway.
*/

#include <string.h>

#include "gpu_remote.h"
#include "psxe_link.h"
#include "enet_link.h"
#include "fsl_debug_console.h"

#include "FreeRTOS.h"
#include "task.h"

#define PAYLOAD_WORDS PSXE_LINK_PAYLOAD_WORDS
#define WINDOW PSXE_LINK_WINDOW

#define CREDIT_TIMEOUT_MS 500 /* no STATUS for this long: the GPU board is gone */
#define READ_TIMEOUT_MS 500   /* a VRAM -> CPU transfer without data for this long */
#define TX_BUSY_TIMEOUT_MS 100

#define RD_FIFO_WORDS 16384u /* VRAM -> CPU data waiting to be read (a 64 KB read) */

/* what the words arriving now belong to */
enum
{
    K_NONE = 0,    /* the next word starts a command */
    K_FIXED,       /* a command of known length */
    K_POLYLINE,    /* until the terminator */
    K_A0_HDR,      /* the two header words of a CPU -> VRAM upload */
    K_A0_DATA,     /* its pixel words */
    K_C0_HDR       /* the two header words of a VRAM -> CPU read */
};

/* the frame being filled; the payload is 4 byte aligned (the Ethernet header is 14 bytes) */
static uint8_t s_frame_mem[2 + PSXE_LINK_FRAME_MAX] __attribute__((section(".bss.$SRAM_DTC"), aligned(4)));
#define FRAME (s_frame_mem + 2)
#define PAYLOAD ((uint32_t *)(FRAME + PSXE_LINK_ETH_HDR + PSXE_LINK_HDR))

static uint32_t s_used;   /* payload words written */
static int32_t s_rec;     /* index of the open GP0 record's header, -1 when none */
static uint16_t s_seq;

/* the shadow parser */
static uint32_t s_kind;
static uint32_t s_left;      /* words still to come (K_FIXED, K_A0_HDR, K_A0_DATA, K_C0_HDR) */
static uint32_t s_idx;       /* index of the word within the command; the command word is 0 */
static uint32_t s_texp_idx;  /* the word carrying the texture page of a textured polygon, 0 if none */
static uint32_t s_env[6];    /* the last E1h..E6h */
static uint32_t s_env_valid; /* bit i: s_env[i] holds one */
static uint32_t s_rd_left;   /* words the GPU board still owes for VRAM -> CPU reads */
static uint32_t s_hold;      /* emit nothing until the next command boundary */
static uint32_t s_cmd;       /* the command word being followed */
static uint32_t s_arg[3];    /* its arguments, for the VRAM copy */

/* the VRAM shadow (gpu->vram, unused by the rasterizer here) and the upload in progress */
static uint16_t *s_vram;
static uint32_t s_a0_x, s_a0_y, s_a0_w, s_a0_xcnt, s_a0_ycnt;

/* the link */
static psx_gpu_t *s_gpu;
static uint32_t s_ready;     /* the GPU board listens (STATUS with READY seen since the link came up) */
static uint32_t s_boot;      /* its boot id: a change means it restarted and lost everything */
static uint32_t s_sent;      /* payload bytes handed to the MAC, in the GPU board's count */
static uint32_t s_consumed;  /* ... of which it has processed */
static uint32_t s_presented; /* frames it has shown */
static uint32_t s_gpu_errors;
static uint32_t s_gpu_flags;
static uint32_t s_frame_no;

/* statistics, for the report */
static uint32_t s_tx_frames, s_tx_bytes, s_credit_waits, s_wait_ms, s_timeouts, s_drops, s_rx_status, s_rx_vram;
static uint32_t s_rep_tx_bytes, s_rep_presented, s_rep_wait_ms;

/* VRAM -> CPU data, in the order it came */
static uint32_t s_rd_fifo[RD_FIFO_WORDS] __attribute__((section(".bss.$BOARD_SDRAM"), aligned(32)));
static uint32_t s_rd_head, s_rd_tail;

static uint8_t s_rx_mem[2 + PSXE_LINK_FRAME_MAX + 32] __attribute__((section(".bss.$BOARD_SDRAM"), aligned(4)));
#define RX_FRAME (s_rx_mem + 2)

static const uint8_t s_mac_cpu[6] = PSXE_LINK_MAC_CPU;
static const uint8_t s_mac_gpu[6] = PSXE_LINK_MAC_GPU;

static void link_replay(void);
static void vram_replay(void);

/* ---- receiving: STATUS and VRAM records ------------------------------------ */

static inline void rd_push(uint32_t w)
{
    const uint32_t next = (s_rd_head + 1u) & (RD_FIFO_WORDS - 1u);

    if (next == s_rd_tail)
        return; /* full: the read will time out and give zeros */

    s_rd_fifo[s_rd_head] = w;
    s_rd_head = next;
}

static inline int rd_empty(void)
{
    return s_rd_head == s_rd_tail;
}

static inline uint32_t rd_pop(void)
{
    const uint32_t w = s_rd_fifo[s_rd_tail];

    s_rd_tail = (s_rd_tail + 1u) & (RD_FIFO_WORDS - 1u);

    return w;
}

static void link_lost(const char *why)
{
    if (s_ready)
        PRINTF("gpu-link: down (%s)\r\n", why);

    s_ready = 0;
    s_used = 0;
    s_rec = -1;
    s_hold = 1;
}

static void on_status(const uint32_t *w, uint32_t n)
{
    const uint32_t consumed = w[PSXE_STATUS_CONSUMED];
    const uint32_t flags = w[PSXE_STATUS_FLAGS];
    const uint32_t boot = (n > PSXE_STATUS_BOOT) ? w[PSXE_STATUS_BOOT] : 1u;

    s_rx_status++;

    if (!(flags & PSXE_STATUS_FLAG_READY))
        return;

    s_gpu_flags = flags;
    s_presented = w[PSXE_STATUS_PRESENTED];
    s_gpu_errors = w[PSXE_STATUS_ERRORS];

    if (!s_ready || (boot != s_boot))
    {
        /* it just started listening, or started over: its counters are the truth now */
        s_boot = boot;
        s_consumed = consumed;
        s_sent = consumed;
        s_used = 0;
        s_rec = -1;
        s_ready = 1;

        PRINTF("gpu-link: up (boot %08x, ring %u KB)\r\n", (unsigned)boot, (unsigned)(w[PSXE_STATUS_RING] / 1024u));

        link_replay();

        return;
    }

    s_consumed = consumed;
}

static void rx_frame(const uint8_t *f, uint32_t len)
{
    if (len < PSXE_LINK_ETH_HDR + PSXE_LINK_HDR)
        return;

    if ((f[12] != (PSXE_LINK_ETHERTYPE >> 8)) || (f[13] != (PSXE_LINK_ETHERTYPE & 0xffu)))
        return;

    const uint8_t *p = f + PSXE_LINK_ETH_HDR;

    if (PSXE_LINK_HDR_VERSION(p) != PSXE_LINK_VERSION)
        return;

    const uint32_t *w = (const uint32_t *)(p + PSXE_LINK_HDR);
    uint32_t words = (len - PSXE_LINK_ETH_HDR - PSXE_LINK_HDR) / 4u;

    while (words)
    {
        const uint32_t h = *w++;
        const uint32_t n = PSXE_REC_LEN(h);

        words--;

        if (n > words)
            break;

        switch (PSXE_REC_TYPE(h))
        {
        case PSXE_REC_STATUS:
            if (n >= PSXE_STATUS_ERRORS + 1u)
                on_status(w, n);
            break;

        case PSXE_REC_VRAM:
            s_rx_vram += n;

            for (uint32_t i = 0; i < n; i++)
                rd_push(w[i]);
            break;

        default:
            break;
        }

        w += n;
        words -= n;
    }
}

static void rx_poll(void)
{
    for (int i = 0; i < 16; i++)
    {
        const int len = enet_link_recv(RX_FRAME, PSXE_LINK_FRAME_MAX + 32);

        if (len <= 0)
            break;

        rx_frame(RX_FRAME, (uint32_t)len);
    }
}

/* ---- sending -------------------------------------------------------------- */

static inline void rec_close(void)
{
    if (s_rec >= 0)
    {
        PAYLOAD[s_rec] |= (s_used - (uint32_t)s_rec - 1u) & 0xffffffu;
        s_rec = -1;
    }
}

static inline void put(uint32_t w)
{
    PAYLOAD[s_used++] = w;
}

/* the credit window: wait until the GPU board has room for this many more bytes */
static int credit_wait(uint32_t bytes)
{
    if ((s_sent - s_consumed) + bytes <= WINDOW)
        return 1;

    const TickType_t t0 = xTaskGetTickCount();

    s_credit_waits++;

    for (;;)
    {
        rx_poll();

        if (!s_ready)
            return 0;

        if ((s_sent - s_consumed) + bytes <= WINDOW)
            break;

        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(CREDIT_TIMEOUT_MS))
        {
            s_timeouts++;
            link_lost("no credit");

            return 0;
        }
    }

    s_wait_ms += (uint32_t)(xTaskGetTickCount() - t0);

    return 1;
}

static void frame_send(void)
{
    rec_close();

    if (!s_used)
        return;

    uint32_t len = PSXE_LINK_ETH_HDR + PSXE_LINK_HDR + s_used * 4u;

    s_used = 0;

    if (!s_ready)
    {
        s_drops++;
        return;
    }

    if (len < 60u)
    {
        memset(FRAME + len, 0, 60u - len);
        len = 60u;
    }

    const uint32_t payload = len - PSXE_LINK_ETH_HDR;

    if (!credit_wait(payload))
    {
        s_drops++;
        return;
    }

    memcpy(FRAME, s_mac_gpu, 6);
    memcpy(FRAME + 6, s_mac_cpu, 6);
    FRAME[12] = PSXE_LINK_ETHERTYPE >> 8;
    FRAME[13] = PSXE_LINK_ETHERTYPE & 0xffu;
    FRAME[14] = PSXE_LINK_VERSION;
    FRAME[15] = 0;
    FRAME[16] = (uint8_t)s_seq;
    FRAME[17] = (uint8_t)(s_seq >> 8);

    const TickType_t t0 = xTaskGetTickCount();

    for (;;)
    {
        const int r = enet_link_send(FRAME, len);

        if (r == 0)
            break;

        if (r != ENET_LINK_BUSY)
        {
            link_lost("send failed");
            s_drops++;

            return;
        }

        /* the transmit ring is full: the wire drains it at 10 MB/s */
        rx_poll();

        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(TX_BUSY_TIMEOUT_MS))
        {
            s_timeouts++;
            link_lost("transmit stuck");
            s_drops++;

            return;
        }
    }

    s_seq++;
    s_sent += payload;
    s_tx_frames++;
    s_tx_bytes += payload;
}

/* room for n more words in the frame, else it goes out first */
static inline void room(uint32_t n)
{
    if (s_used + n > PAYLOAD_WORDS)
        frame_send();
}

static inline void emit_gp0(uint32_t w, int boundary)
{
    if ((s_rec < 0) || (s_used >= PAYLOAD_WORDS))
    {
        room(2);
        s_rec = (int32_t)s_used;
        put(PSXE_REC_HDR(PSXE_REC_GP0, boundary ? PSXE_REC_GP0_BOUNDARY : 0u, 0u));
    }

    put(w);
}

static void emit_gp1(uint32_t w)
{
    rec_close();
    room(2);
    put(PSXE_REC_HDR(PSXE_REC_GP1, 0u, 1u));
    put(w);
}

/* the GPU board starts from nothing: its display state, then the environment */
static void link_replay(void)
{
    psx_gpu_t *const gpu = s_gpu;

    rec_close();
    room(1);
    put(PSXE_REC_HDR(PSXE_REC_RESET, 0u, 0u));

    if (gpu)
    {
        emit_gp1(0x08000000u | (gpu->display_mode & 0xffffffu));
        emit_gp1(0x05000000u | (gpu->disp_x & 0x3ffu) | ((gpu->disp_y & 0x1ffu) << 10));
        emit_gp1(0x06000000u | (gpu->disp_x1 & 0xfffu) | ((gpu->disp_x2 & 0xfffu) << 12));
        emit_gp1(0x07000000u | (gpu->disp_y1 & 0x1ffu) | ((gpu->disp_y2 & 0x1ffu) << 10));
        emit_gp1(0x03000000u | ((gpu->gpustat >> 23) & 1u));
    }

    for (uint32_t i = 0; i < 6u; i++)
    {
        if (s_env_valid & (1u << i))
        {
            room(2);
            put(PSXE_REC_HDR(PSXE_REC_GP0, PSXE_REC_GP0_BOUNDARY, 1u));
            put(s_env[i]);
        }
    }

    vram_replay();

    frame_send();

    /* the stream may be inside a command: nothing more until the next one starts */
    s_hold = (s_kind != K_NONE);
}

/* ---- the VRAM shadow --------------------------------------------------------- */

/* one halfword of a CPU -> VRAM upload, where gpu.c would put it */
static inline void shadow_half(uint16_t v)
{
    s_vram[(((s_a0_y + s_a0_ycnt) & 0x1ffu) << 10) + ((s_a0_x + s_a0_xcnt) & 0x3ffu)] = v;

    if (++s_a0_xcnt == s_a0_w)
    {
        s_a0_xcnt = 0;
        s_a0_ycnt++;
    }
}

static inline void shadow_word(uint32_t w)
{
    if (!s_vram)
        return;

    shadow_half((uint16_t)w);
    shadow_half((uint16_t)(w >> 16));
}

static void shadow_words(const uint32_t *src, uint32_t n)
{
    if (!s_vram)
        return;

    while (n)
    {
        /* a whole row that does not wrap: one copy */
        if ((s_a0_xcnt == 0u) && !(s_a0_w & 1u) && ((s_a0_x + s_a0_w) <= 1024u) && (n >= (s_a0_w >> 1)))
        {
            memcpy(&s_vram[(((s_a0_y + s_a0_ycnt) & 0x1ffu) << 10) + s_a0_x], src, s_a0_w * 2u);

            src += s_a0_w >> 1;
            n -= s_a0_w >> 1;
            s_a0_ycnt++;
        }
        else
        {
            shadow_word(*src++);
            n--;
        }
    }
}

/* GP0(80h) VRAM -> VRAM, as gpu.c does it */
static void shadow_copy(void)
{
    if (!s_vram)
        return;

    const uint32_t srcx = s_arg[0] & 0xffffu;
    const uint32_t srcy = s_arg[0] >> 16;
    const uint32_t dstx = s_arg[1] & 0xffffu;
    const uint32_t dsty = s_arg[1] >> 16;
    const uint32_t xsiz = s_arg[2] & 0xffffu;
    const uint32_t ysiz = s_arg[2] >> 16;

    for (uint32_t y = 0; y < ysiz; y++)
    {
        if (((dsty + y) >= 512u) || ((srcy + y) >= 512u))
            continue;

        for (uint32_t x = 0; x < xsiz; x++)
        {
            if (((dstx + x) < 1024u) && ((srcx + x) < 1024u))
                s_vram[(dstx + x) + ((dsty + y) << 10)] = s_vram[(srcx + x) + ((srcy + y) << 10)];
        }
    }
}

/* the whole shadow to the GPU board, eight rows per upload command */
static void vram_replay(void)
{
    if (!s_vram)
        return;

    for (uint32_t y = 0; y < 512u; y += 8u)
    {
        rec_close();
        room(4);
        s_rec = (int32_t)s_used;
        put(PSXE_REC_HDR(PSXE_REC_GP0, PSXE_REC_GP0_BOUNDARY, 0u));
        put(0xa0000000u);
        put(y << 16);
        put((8u << 16) | 1024u);

        const uint32_t *src = (const uint32_t *)&s_vram[y << 10];
        uint32_t left = 4096u;

        while (left)
        {
            if (s_rec < 0)
            {
                room(2);
                s_rec = (int32_t)s_used;
                put(PSXE_REC_HDR(PSXE_REC_GP0, 0u, 0u));
            }

            const uint32_t space = PAYLOAD_WORDS - s_used;

            if (!space)
            {
                frame_send();
                continue;
            }

            const uint32_t chunk = (left < space) ? left : space;

            memcpy(&PAYLOAD[s_used], src, chunk * 4u);

            s_used += chunk;
            src += chunk;
            left -= chunk;
        }

        rec_close();
        frame_send();

        /* not the whole megabyte at line rate: a little at a time, as the GPU board takes it */
        const TickType_t t0 = xTaskGetTickCount();

        while (s_ready && ((s_sent - s_consumed) > 65536u))
        {
            rx_poll();

            if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(CREDIT_TIMEOUT_MS))
            {
                link_lost("no credit (restore)");
                return;
            }
        }
    }
}

/* ---- the GP0 stream --------------------------------------------------------- */

void gpu_remote_reset(psx_gpu_t *gpu)
{
    s_gpu = gpu;
    s_vram = gpu->vram;
    s_kind = K_NONE;
    s_left = 0;
    s_idx = 0;
    s_texp_idx = 0;
    s_env_valid = 0;
    s_rd_left = 0;
    s_rd_head = 0;
    s_rd_tail = 0;

    if (s_ready)
        link_replay();
    else
        s_hold = 1;
}

/* the size words of A0h/C0h, as gpu.c reads them: words of pixel data */
static inline uint32_t transfer_words(uint32_t size)
{
    const uint32_t xsiz = (((size & 0xffffu) - 1u) & 0x3ffu) + 1u;
    const uint32_t ysiz = (((size >> 16) - 1u) & 0x1ffu) + 1u;

    return (((xsiz * ysiz) + 1u) & 0xfffffffeu) / 2u;
}

uint32_t gpu_remote_gp0(psx_gpu_t *gpu, uint32_t w)
{
    uint32_t env = 0;
    int boundary = 0;

    switch (s_kind)
    {
    case K_NONE:
    {
        const uint32_t cmd = w >> 24;

        boundary = 1;
        s_idx = 0;
        s_texp_idx = 0;
        s_left = 0;
        s_cmd = cmd;

        switch (cmd >> 5)
        {
        case 0: /* 00h-1Fh: fill rectangle has two more words, the rest one */
            s_left = (cmd == 0x02u) ? 2u : 0u;
            break;

        case 1: /* 20h-3Fh: polygons */
        {
            const uint32_t verts = (cmd & 0x08u) ? 4u : 3u;
            const uint32_t textured = (cmd & 0x04u) ? 1u : 0u;
            const uint32_t shaded = (cmd & 0x10u) ? 1u : 0u;

            s_left = verts * (1u + textured + shaded) + (shaded ? 0u : 1u) - 1u;

            if (textured)
                s_texp_idx = shaded ? 5u : 4u;
        }
        break;

        case 2: /* 40h-5Fh: lines; a polyline runs until its terminator */
            if (cmd & 0x08u)
                s_kind = K_POLYLINE;
            else
                s_left = (cmd & 0x10u) ? 3u : 2u;
            break;

        case 3: /* 60h-7Fh: rectangles */
            s_left = 1u + ((cmd & 0x04u) ? 1u : 0u) + ((((cmd >> 3) & 3u) == 0u) ? 1u : 0u);
            break;

        case 4: /* 80h-9Fh: VRAM -> VRAM */
            s_left = 3u;
            break;

        case 5: /* A0h-BFh: CPU -> VRAM */
            s_kind = K_A0_HDR;
            s_left = 2u;
            break;

        case 6: /* C0h-DFh: VRAM -> CPU */
            s_kind = K_C0_HDR;
            s_left = 2u;
            break;

        default: /* E0h-FFh: the environment, one word each */
            if ((cmd >= 0xe1u) && (cmd <= 0xe6u))
            {
                s_env[cmd - 0xe1u] = w;
                s_env_valid |= 1u << (cmd - 0xe1u);
                env = cmd;
            }
            break;
        }

        if ((s_kind == K_NONE) && s_left)
            s_kind = K_FIXED;
    }
    break;

    case K_FIXED:
        s_idx++;

        if (((s_cmd >> 5) == 4u) && (s_idx <= 3u))
            s_arg[s_idx - 1u] = w;

        if (s_idx == s_texp_idx)
        {
            /* the texture page of a textured polygon lands in GPUSTAT (gpu.c does the same) */
            const uint32_t texp = w >> 16;

            gpu->texp_x = (texp & 0xfu) << 6;
            gpu->texp_y = (texp & 0x10u) << 4;
            gpu->texp_d = (texp >> 7) & 0x3u;
            gpu->gpustat = (gpu->gpustat & 0xfffffe00u) | (texp & 0x1ffu);
        }

        if (--s_left == 0u)
        {
            s_kind = K_NONE;

            if ((s_cmd >> 5) == 4u)
                shadow_copy();
        }
        break;

    case K_POLYLINE:
        if ((w & 0xf000f000u) == 0x50005000u)
            s_kind = K_NONE;
        break;

    case K_A0_HDR:
        if (--s_left == 0u)
        {
            s_a0_w = (((w & 0xffffu) - 1u) & 0x3ffu) + 1u;
            s_a0_xcnt = 0;
            s_a0_ycnt = 0;
            s_left = transfer_words(w);
            s_kind = s_left ? K_A0_DATA : K_NONE;
        }
        else
        {
            s_a0_x = w & 0x3ffu;
            s_a0_y = (w >> 16) & 0x1ffu;
        }
        break;

    case K_A0_DATA:
        shadow_word(w);

        if (--s_left == 0u)
            s_kind = K_NONE;
        break;

    case K_C0_HDR:
        if (--s_left == 0u)
        {
            s_rd_left += transfer_words(w);
            s_kind = K_NONE;
        }
        break;

    default:
        s_kind = K_NONE;
        break;
    }

    if (s_ready)
    {
        if (s_hold)
        {
            if (!boundary)
                return env;

            s_hold = 0;
        }

        emit_gp0(w, boundary);
    }

    return env;
}

uint32_t gpu_remote_gp0_bulk(psx_gpu_t *gpu, const uint32_t *src, uint32_t words)
{
    (void)gpu;

    if ((s_kind != K_A0_DATA) || !words)
        return 0;

    const uint32_t n = (words < s_left) ? words : s_left;
    uint32_t left = n;

    shadow_words(src, n);

    if (s_ready && !s_hold)
    {
        while (left)
        {
            if (s_rec < 0)
            {
                room(2);
                s_rec = (int32_t)s_used;
                put(PSXE_REC_HDR(PSXE_REC_GP0, 0u, 0u));
            }

            const uint32_t space = PAYLOAD_WORDS - s_used;

            if (!space)
            {
                frame_send();
                continue;
            }

            const uint32_t chunk = (left < space) ? left : space;

            memcpy(&PAYLOAD[s_used], src, chunk * 4u);

            s_used += chunk;
            src += chunk;
            left -= chunk;
        }
    }

    s_left -= n;

    if (!s_left)
        s_kind = K_NONE;

    return n;
}

void gpu_remote_gp1(psx_gpu_t *gpu, uint32_t w)
{
    (void)gpu;

    if (s_ready)
        emit_gp1(w);
}

/* ---- VRAM -> CPU ------------------------------------------------------------ */

uint32_t gpu_remote_reading(void)
{
    return s_rd_left;
}

uint32_t gpu_remote_read_vram(psx_gpu_t *gpu)
{
    (void)gpu;

    if (!s_rd_left)
        return 0;

    s_rd_left--;

    if (!s_ready)
        return 0;

    if (rd_empty())
    {
        /* the read command may still be in the frame being filled */
        frame_send();

        const TickType_t t0 = xTaskGetTickCount();

        while (rd_empty())
        {
            rx_poll();

            if (!s_ready)
                return 0;

            if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(READ_TIMEOUT_MS))
            {
                s_timeouts++;
                s_rd_left = 0;

                PRINTF("gpu-link: VRAM read timed out\r\n");

                return 0;
            }
        }
    }

    return rd_pop();
}

/* ---- the vertical blank -------------------------------------------------------- */

void gpu_remote_vblank(psx_gpu_t *gpu, uint32_t field)
{
    (void)gpu;

    s_frame_no++;

    if (s_ready)
    {
        rec_close();
        room(2);
        put(PSXE_REC_HDR(PSXE_REC_VBLANK, 0u, 1u));
        put((field & 1u) | (s_frame_no << 8));
        frame_send();
    }

    rx_poll();

    /* the PHY, ten times a second */
    if ((s_frame_no % 6u) == 0u)
    {
        if (!enet_link_poll_phy() && s_ready)
            link_lost("no carrier");
    }
}

uint32_t gpu_remote_presented(void)
{
    return s_presented;
}

void gpu_remote_report(void)
{
    const uint32_t kb = (s_tx_bytes - s_rep_tx_bytes) / 1024u;
    const uint32_t pres = s_presented - s_rep_presented;
    const uint32_t wait = s_wait_ms - s_rep_wait_ms;

    s_rep_tx_bytes = s_tx_bytes;
    s_rep_presented = s_presented;
    s_rep_wait_ms = s_wait_ms;

    PRINTF("gpu-link: %s tx=%uKB/s wait=%ums drop=%u to=%u | gpu: shown=%u/s err=%u%s | rd=%u\r\n",
           s_ready ? "up" : (enet_link_up() ? "carrier" : "down"), (unsigned)kb, (unsigned)wait,
           (unsigned)s_drops, (unsigned)s_timeouts, (unsigned)pres, (unsigned)s_gpu_errors,
           (s_gpu_flags & PSXE_STATUS_FLAG_RESYNC) ? " RESYNC" : "", (unsigned)s_rx_vram);
}
