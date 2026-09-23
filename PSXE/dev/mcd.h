#ifndef MCD_H
#define MCD_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MCD_MEMORY_SIZE 0x20000 // 128 KB

enum
{
    MCD_STATE_TX_HIZ = 0,
    MCD_STATE_TX_FLG,
    MCD_STATE_TX_ID1,
    MCD_STATE_TX_ID2,
    MCD_R_STATE_RX_MSB,
    MCD_R_STATE_RX_LSB,
    MCD_R_STATE_TX_ACK1,
    MCD_R_STATE_TX_ACK2,
    MCD_R_STATE_TX_MSB,
    MCD_R_STATE_TX_LSB,
    MCD_R_STATE_TX_DATA,
    MCD_R_STATE_TX_CHK,
    MCD_R_STATE_TX_MEB,
    MCD_W_STATE_RX_MSB,
    MCD_W_STATE_RX_LSB,
    MCD_W_STATE_RX_DATA,
    MCD_W_STATE_RX_CHK,
    MCD_W_STATE_TX_ACK1,
    MCD_W_STATE_TX_ACK2,
    MCD_W_STATE_TX_MEB,
    MCD_S_STATE_TX_ACK1,
    MCD_S_STATE_TX_ACK2,
    MCD_S_STATE_TX_DAT0,
    MCD_S_STATE_TX_DAT1,
    MCD_S_STATE_TX_DAT2,
    MCD_S_STATE_TX_DAT3
};

typedef struct
{
    const char *path;
    uint8_t *buf;
    uint8_t flag;
    uint16_t msb;
    uint16_t lsb;
    uint32_t addr; /* byte offset into the 128 KB card (a uint16_t lost frames 512 and up) */
    uint8_t rx_data;
    int32_t pending_bytes;
    char mode;
    int32_t state;
    uint8_t tx_data;
    int32_t tx_data_ready;
    uint8_t checksum;

    /* The card is kept in RAM and saved to its file on the SD card a moment
       after the game stops writing (psx_mcd_tick): only the 128 byte frames
       that changed, one bit each. */
    uint8_t dirty[MCD_MEMORY_SIZE / 128 / 8];
    uint32_t dirty_frames;
    int32_t written;  /* a frame was written since the last tick */
    uint32_t last_ms; /* when the last write was seen */
} psx_mcd_t;

psx_mcd_t *psx_mcd_create(void);

/* Loads the card from the file at path; a missing file becomes a new,
   formatted card (written out at once). 0 on success. */
int32_t psx_mcd_init(psx_mcd_t *, const char *);

/* Called now and then with a millisecond clock: saves the changed frames once
   the game has not written for half a second. */
void psx_mcd_tick(psx_mcd_t *, uint32_t now_ms);

/* Saves the changed frames now; how many were saved, < 0 on an SD card error. */
int32_t psx_mcd_flush(psx_mcd_t *);
uint8_t psx_mcd_read(psx_mcd_t *);
void psx_mcd_write(psx_mcd_t *, uint8_t);
int32_t psx_mcd_query(psx_mcd_t *);
void psx_mcd_reset(psx_mcd_t *);
void psx_mcd_destroy(psx_mcd_t *);

#endif