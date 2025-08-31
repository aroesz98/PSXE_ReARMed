#ifndef MDEC_H
#define MDEC_H

#include <stdint.h>
#include <stddef.h>

#include "../log.h"

#define PSX_MDEC_SIZE 0x8
#define PSX_MDEC_BEGIN 0x1f801820
#define PSX_MDEC_END 0x1f801827

#define MDEC_SCALE_TABLE_SIZE 64
#define MDEC_QUANT_TABLE_SIZE 64

enum
{
    MDEC_RECV_CMD,
    MDEC_RECV_BLOCK,
    MDEC_RECV_QUANT,
    MDEC_RECV_QUANT_COLOR,
    MDEC_RECV_SCALE
};

/*
  31    Data-Out Fifo Empty (0=No, 1=Empty)
  30    Data-In Fifo Full   (0=No, 1=Full, or Last word received)
  29    Command Busy  (0=Ready, 1=Busy receiving or processing parameters)
  28    Data-In Request  (set when DMA0 enabled and ready to receive data)
  27    Data-Out Request (set when DMA1 enabled and ready to send data)
  26-25 Data Output Depth  (0=4bit, 1=8bit, 2=24bit, 3=15bit)      ;CMD.28-27
  24    Data Output Signed (0=Unsigned, 1=Signed)                  ;CMD.26
  23    Data Output Bit15  (0=Clear, 1=Set) (for 15bit depth only) ;CMD.25
  22-19 Not used (seems to be always zero)
  18-16 Current Block (0..3=Y1..Y4, 4=Cr, 5=Cb) (or for mono: always 4=Y)
  15-0  Number of Parameter Words remaining minus 1  (FFFFh=None)  ;CMD.Bit0-15
*/

enum
{
    MDEC_CMD_NOP,
    MDEC_CMD_DECODE,
    MDEC_CMD_SET_QT,
    MDEC_CMD_SET_ST
};

typedef struct
{
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    uint32_t cmd;

    int32_t state;
    int32_t data_remaining;
    int32_t index;

    uint32_t *input;
    int32_t input_index;
    size_t input_size;

    uint8_t *output;
    int32_t output_index;
    uint32_t output_words_remaining;

    uint32_t words_remaining;
    int32_t current_block;
    int32_t output_bit15;
    int32_t output_signed;
    int32_t output_depth;
    int32_t input_request;
    int32_t output_request;
    int32_t busy;
    int32_t input_full;
    int32_t output_empty;

    int32_t enable_dma0;
    int32_t enable_dma1;

    int32_t recv_color;
    uint8_t uv_quant_table[MDEC_QUANT_TABLE_SIZE];
    uint8_t y_quant_table[MDEC_QUANT_TABLE_SIZE];
    int16_t scale_table[MDEC_SCALE_TABLE_SIZE];

    int16_t yblk[64];
    int16_t crblk[64];
    int16_t cbblk[64];

    uint32_t status;
} psx_mdec_t;

psx_mdec_t *psx_mdec_create(void);
void psx_mdec_init(psx_mdec_t *);
uint32_t psx_mdec_read32(psx_mdec_t *, uint32_t);
uint16_t psx_mdec_read16(psx_mdec_t *, uint32_t);
uint8_t psx_mdec_read8(psx_mdec_t *, uint32_t);
void psx_mdec_write32(psx_mdec_t *, uint32_t, uint32_t);
void psx_mdec_write16(psx_mdec_t *, uint32_t, uint16_t);
void psx_mdec_write8(psx_mdec_t *, uint32_t, uint8_t);
void psx_mdec_destroy(psx_mdec_t *);

// MDEC JPEG decode function
int32_t mdec_jpeg_decode(const uint8_t *jpeg_data, size_t jpeg_size, uint8_t *output_buffer,
                         size_t output_width, size_t output_height, int32_t output_depth);

typedef void (*mdec_fn_t)(psx_mdec_t *);

#endif