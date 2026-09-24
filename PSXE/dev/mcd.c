#include "mcd.h"
#include "../log.h"

/* FATFS includes */
#include "ff.h"

psx_mcd_t *psx_mcd_create(void)
{
    return (psx_mcd_t *)malloc(sizeof(psx_mcd_t));
}

/* a frame's last byte: the XOR of the other 127 */
static void mcd_frame_checksum(uint8_t *frame)
{
    uint8_t x = 0;

    for (int i = 0; i < 127; i++)
        x ^= frame[i];

    frame[127] = x;
}

/*
    A freshly formatted card, as the PSX's own formatting leaves it (the layout
    in the PSX-SPX documentation): block 0 holds the header frame ("MC"), 15
    directory frames with every block free (A0h, no next block), 20 frames of
    an empty broken sector list, and the header again as the last frame; the
    15 save blocks are zero.
*/
static void mcd_format(uint8_t *buf)
{
    memset(buf, 0, MCD_MEMORY_SIZE);

    buf[0] = 'M';
    buf[1] = 'C';
    mcd_frame_checksum(&buf[0]);

    for (int f = 1; f <= 15; f++)
    {
        uint8_t *d = &buf[f * 128];

        d[0] = 0xa0; /* free, freshly formatted */
        d[8] = 0xff; /* no next block */
        d[9] = 0xff;
        mcd_frame_checksum(d);
    }

    for (int f = 16; f <= 35; f++)
    {
        uint8_t *d = &buf[f * 128];

        memset(d, 0xff, 4); /* no broken sector */
        d[8] = 0xff;
        d[9] = 0xff;
        mcd_frame_checksum(d);
    }

    memcpy(&buf[63 * 128], &buf[0], 128);
}

int32_t psx_mcd_init(psx_mcd_t *mcd, const char *path)
{
    memset(mcd, 0, sizeof(psx_mcd_t));

    mcd->state = MCD_STATE_TX_HIZ;
    mcd->flag = 0x08;
    mcd->path = path;
    mcd->buf = malloc(MCD_MEMORY_SIZE);
    mcd->tx_data_ready = 0;

    if (!mcd->buf)
        return 3;

    memset(mcd->buf, 0, MCD_MEMORY_SIZE);

    if (!path)
        return 0;

    FIL file;
    FRESULT res;

    res = f_open(&file, path, FA_READ);

    if ((res == FR_NO_FILE) || (res == FR_NO_PATH))
    {
        /* no card yet: a new one, saved whole at once */
        mcd_format(mcd->buf);

        memset(mcd->dirty, 0xff, sizeof(mcd->dirty));
        mcd->dirty_frames = MCD_MEMORY_SIZE / 128;

        const int32_t n = psx_mcd_flush(mcd);

        PRINTF("Memory card %s: new, formatted%s\r\n", path, (n < 0) ? " (could not be written)" : "");

        return 0;
    }

    if (res != FR_OK)
        return 1;

    UINT bytesRead;
    res = f_read(&file, mcd->buf, MCD_MEMORY_SIZE, &bytesRead);
    if (res != FR_OK)
    {
        f_close(&file);
        return 2;
    }

    f_close(&file);

    PRINTF("Memory card %s: %u bytes loaded\r\n", path, (unsigned)bytesRead);

    return 0;
}

int32_t psx_mcd_flush(psx_mcd_t *mcd)
{
    if (!mcd->dirty_frames || !mcd->path)
        return 0;

    FIL file;

    if (f_open(&file, mcd->path, FA_WRITE | FA_OPEN_ALWAYS) != FR_OK)
    {
        mcd->save_failed = 1;

        return -1;
    }

    int32_t saved = 0;
    int32_t err = 0;

    /* runs of changed frames, one seek and one write each */
    for (uint32_t f = 0; f < (MCD_MEMORY_SIZE / 128u);)
    {
        if (!(mcd->dirty[f >> 3] & (1u << (f & 7u))))
        {
            f++;
            continue;
        }

        uint32_t end = f;

        while ((end < (MCD_MEMORY_SIZE / 128u)) && (mcd->dirty[end >> 3] & (1u << (end & 7u))))
            end++;

        UINT done = 0;

        if ((f_lseek(&file, f * 128u) != FR_OK) ||
            (f_write(&file, &mcd->buf[f * 128u], (end - f) * 128u, &done) != FR_OK) || (done != (end - f) * 128u))
        {
            err = 1;
            break;
        }

        saved += (int32_t)(end - f);
        f = end;
    }

    if (f_close(&file) != FR_OK)
        err = 1;

    if (err)
    {
        mcd->save_failed = 1;

        return -1;
    }

    memset(mcd->dirty, 0, sizeof(mcd->dirty));
    mcd->dirty_frames = 0;
    mcd->save_failed = 0;
    mcd->saves++;

    return saved;
}

void psx_mcd_tick(psx_mcd_t *mcd, uint32_t now_ms)
{
    if (mcd->written)
    {
        mcd->written = 0;
        mcd->last_ms = now_ms;

        return;
    }

    /* a save is many frames in a row: wait for the game to finish it */
    if (mcd->dirty_frames && ((now_ms - mcd->last_ms) >= 500u))
    {
        const int32_t n = psx_mcd_flush(mcd);

        if (n < 0)
            mcd->last_ms = now_ms; /* try again in a moment */

        PRINTF("Memory card %s: %d frames saved%s\r\n", mcd->path, (int)((n < 0) ? 0 : n),
               (n < 0) ? " - SD card write FAILED" : "");
    }
}

uint8_t psx_mcd_read(psx_mcd_t *mcd)
{
    switch (mcd->state)
    {
    case MCD_STATE_TX_HIZ:
        mcd->tx_data = 0xff;
        break;
    case MCD_STATE_TX_FLG:
        /* bit 3: no write since the card was inserted - cleared by the first
           good write (below), not by reading it */
        mcd->tx_data = mcd->flag;

        /* not a command a card knows: the FLAG byte, then it goes quiet (no
           acknowledge) */
        if ((mcd->mode != 'R') && (mcd->mode != 'W') && (mcd->mode != 'S'))
        {
            mcd->tx_data_ready = 0;
            mcd->state = MCD_STATE_TX_HIZ;

            return mcd->tx_data;
        }
        break;
    case MCD_STATE_TX_ID1:
        mcd->tx_data = 0x5a;
        break;
    case MCD_STATE_TX_ID2:
    {
        mcd->tx_data_ready = 1;
        mcd->tx_data = 0x5d;

        switch (mcd->mode)
        {
        case 'R':
            mcd->state = MCD_R_STATE_RX_MSB;
            break;
        case 'W':
            mcd->state = MCD_W_STATE_RX_MSB;
            break;
        case 'S':
            mcd->state = MCD_S_STATE_TX_ACK1;
            break;
        default:
            /* not a command a card knows: it goes quiet */
            mcd->tx_data_ready = 0;
            mcd->state = MCD_STATE_TX_HIZ;
            break;
        }

        // PRINTF("mcd read %02x\n", mcd->tx_data);

        // log_set_quiet(0);
        // log_fatal("mcd read %02x", mcd->tx_data);
        // log_set_quiet(1);

        return mcd->tx_data;
    }
    break;

    // Read states
    case MCD_R_STATE_RX_MSB:
        mcd->tx_data = 0x00;
        break;
    case MCD_R_STATE_RX_LSB:
        mcd->tx_data = mcd->msb;
        break;
    case MCD_R_STATE_TX_ACK1:
        mcd->tx_data = 0x5c;
        break;
    case MCD_R_STATE_TX_ACK2:
        mcd->tx_data = 0x5d;
        break;
    case MCD_R_STATE_TX_MSB:
        mcd->tx_data = mcd->msb;
        mcd->checksum = mcd->msb;
        break;
    case MCD_R_STATE_TX_LSB:
        mcd->tx_data = mcd->lsb;
        mcd->checksum ^= mcd->lsb;
        mcd->pending_bytes = 128;
        break;
    case MCD_R_STATE_TX_DATA:
    {
        --mcd->pending_bytes;

        uint8_t data = mcd->buf[mcd->addr++];

        mcd->checksum ^= data;

        if (!mcd->pending_bytes)
        {
            mcd->tx_data = data;

            break;
        }

        // PRINTF("mcd read %02x\n", data);

        // log_set_quiet(0);
        // log_fatal("mcd read %02x", data);
        // log_set_quiet(1);

        return data;
    }
    break;
    case MCD_R_STATE_TX_CHK:
        mcd->tx_data = mcd->checksum;
        break;
    case MCD_R_STATE_TX_MEB:
    {
        mcd->tx_data_ready = 0;
        mcd->state = MCD_STATE_TX_HIZ;

        // log_set_quiet(0);
        // log_fatal("mcd read %02x", 'G');
        // log_set_quiet(1);

        // PRINTF("mcd read %02x\n", 'G');

        return 'G';
    }
    break;

    /* Write states */
    case MCD_W_STATE_RX_MSB:
        mcd->tx_data = 0x00;
        break;
    case MCD_W_STATE_RX_LSB:
        /* 128 data bytes follow, as for a read: with 127 the last byte of every
           frame was never stored and the card's acknowledge came a byte early,
           which the BIOS takes for a failed write */
        mcd->tx_data = mcd->msb;
        mcd->pending_bytes = 128;
        break;
    case MCD_W_STATE_RX_DATA:
    {
        --mcd->pending_bytes;

        mcd->buf[mcd->addr++] = mcd->rx_data;

        if (!mcd->pending_bytes)
        {
            mcd->tx_data = mcd->rx_data;

            break;
        }

        // PRINTF("mcd read %02x\n", mcd->rx_data);

        // log_set_quiet(0);
        // log_fatal("mcd read %02x", mcd->rx_data);
        // log_set_quiet(1);

        return mcd->rx_data;
    }
    break;
    case MCD_W_STATE_RX_CHK:
        mcd->tx_data = mcd->rx_data;
        break;
    case MCD_W_STATE_TX_ACK1:
        mcd->tx_data = 0x5c;
        break;
    case MCD_W_STATE_TX_ACK2:
        mcd->tx_data = 0x5d;
        break;
    case MCD_W_STATE_TX_MEB:
    {
        mcd->tx_data_ready = 0;
        mcd->state = MCD_STATE_TX_HIZ;
        mcd->flag &= (uint8_t)~0x08u;

        // log_set_quiet(0);
        // log_fatal("mcd read %02x", 'G');
        // log_set_quiet(1);

        // PRINTF("mcd read %02x\n", 'G');

        return 'G';
    }
    break;

    /* Get ID ('S'): what a standard 128 KB card answers (PSX-SPX), then the
       transfer ends. The states were there, the answers were not: the state
       ran off the end and the card never let go of the port. */
    case MCD_S_STATE_TX_ACK1:
        mcd->tx_data = 0x5c;
        break;
    case MCD_S_STATE_TX_ACK2:
        mcd->tx_data = 0x5d;
        break;
    case MCD_S_STATE_TX_DAT0:
        mcd->tx_data = 0x04;
        break;
    case MCD_S_STATE_TX_DAT1:
        mcd->tx_data = 0x00;
        break;
    case MCD_S_STATE_TX_DAT2:
        mcd->tx_data = 0x00;
        break;
    case MCD_S_STATE_TX_DAT3:
        mcd->tx_data_ready = 0;
        mcd->state = MCD_STATE_TX_HIZ;

        return 0x80;

    default:
        /* nowhere a transfer can be: end it */
        mcd->tx_data_ready = 0;
        mcd->state = MCD_STATE_TX_HIZ;

        return 0xff;
    }

    mcd->tx_data_ready = 1;
    mcd->state++;

    // log_set_quiet(0);
    // log_fatal("mcd read %02x", mcd->tx_data);
    // log_set_quiet(1);

    // PRINTF("mcd read %02x\n", mcd->tx_data);

    return mcd->tx_data;
}

void psx_mcd_write(psx_mcd_t *mcd, uint8_t data)
{
    switch (mcd->state)
    {
    case MCD_STATE_TX_FLG:
        mcd->mode = data;
        break;
    case MCD_R_STATE_RX_MSB:
        mcd->msb = data;
        break;
    case MCD_R_STATE_RX_LSB:
    {
        mcd->lsb = data;
        mcd->addr = (uint32_t)(((mcd->msb << 8) | mcd->lsb) & 0x3ffu) << 7;
    }
    break;
    case MCD_W_STATE_RX_MSB:
        mcd->msb = data;
        break;
    case MCD_W_STATE_RX_LSB:
    {
        mcd->lsb = data;

        const uint32_t frame = ((uint32_t)(mcd->msb << 8) | mcd->lsb) & 0x3ffu;

        mcd->addr = frame << 7;

        /* this frame goes to the SD card soon (psx_mcd_tick) */
        if (!(mcd->dirty[frame >> 3] & (1u << (frame & 7u))))
        {
            mcd->dirty[frame >> 3] |= (uint8_t)(1u << (frame & 7u));
            mcd->dirty_frames++;
        }

        mcd->written = 1;
    }
    break;
    case MCD_W_STATE_RX_DATA:
        mcd->rx_data = data;
        break;
    case MCD_W_STATE_RX_CHK: /* Don't care */
        break;
    }
}

int32_t psx_mcd_query(psx_mcd_t *mcd)
{
    return mcd->tx_data_ready;
}

void psx_mcd_reset(psx_mcd_t *mcd)
{
    mcd->state = MCD_STATE_TX_HIZ;
}

void psx_mcd_destroy(psx_mcd_t *mcd)
{
    if (mcd->buf)
        (void)psx_mcd_flush(mcd);

    free(mcd->buf);
    free(mcd);
}