/*
    Sound out of the EVKB's headphone jack.

    The WM8960 codec is the I2S master: from the 12 MHz MCLK the SAI gives it
    (audio PLL 768 MHz / 4 / 16), its own PLL makes the 44.1 kHz frame clock the
    PSX SPU runs at, so the samples go out as they are made. SAI1 is the slave
    and takes its samples from eDMA channel 0, which loops over a buffer of two
    halves in non-cacheable RAM without end; at each half the interrupt refills
    the half just played from a ring the emulator fills (audio_out_push). What
    the ring does not have when it is needed is played as silence.

    The ring is kept short: the emulator makes the samples at the pace of the
    emulated time, which the frame limiter holds to real time, so they come in
    as fast as they go out; a ring that fills beyond AUDIO_RING_MAX would only
    add delay, and what would go beyond it is dropped.

    Drivers: fsl_sai, fsl_edma, fsl_dmamux, fsl_wm8960 (+ fsl_codec_i2c and the
    LPI2C adapter) from the MCUXpresso SDK for this board. Pins as in the SDK's
    sai/edma_transfer example.
*/
#include "audio_out.h"

#include <string.h>

#include "fsl_common.h"
#include "fsl_clock.h"
#include "fsl_iomuxc.h"
#include "fsl_sai.h"
#include "fsl_edma.h"
#include "fsl_dmamux.h"
#include "fsl_wm8960.h"
#include "fsl_debug_console.h"

#include "FreeRTOS.h"
#include "task.h"

/*
    A build without the frame limiter (PSXE_FRAME_LIMIT=0, for measuring) runs
    the emulation as fast as it can: the sound is made and queued as always, so
    that it costs what it costs, but it is not played - at two or three times
    real speed it would only be torn to pieces - and nothing waits for it.
*/
#if defined(PSXE_FRAME_LIMIT) && (PSXE_FRAME_LIMIT == 0)
#define AUDIO_PACED 0
#else
#define AUDIO_PACED 1
#endif

void psx_platform_audio_out(const int16_t *frames, uint32_t count);

#define AUDIO_DMA_CH 0u
#define AUDIO_HALF_FRAMES 256u   /* 5.8 ms per half */
#define AUDIO_RING_FRAMES 4096u  /* power of two */
#define AUDIO_RING_MAX 2048u     /* ~46 ms: fuller than that is only delay */

/* one stereo frame = two int16 = one uint32 */
static uint32_t __attribute__((section("NonCacheable"), aligned(32))) s_dma[2u * AUDIO_HALF_FRAMES];
static uint32_t __attribute__((section(".bss.$BOARD_SDRAM"), aligned(32))) s_ring[AUDIO_RING_FRAMES];

static volatile uint32_t s_wr; /* frames pushed, ever (the emulator) */
static volatile uint32_t s_rd; /* frames taken, ever (the interrupt) */
static volatile uint32_t s_underruns;
static int32_t s_ok;

static wm8960_handle_t s_codec;

/*
    After the ring has run dry it has to fill up again before anything is
    played: the emulator makes the samples exactly as fast as they are played
    (the frame limiter), so a ring left empty would stay near empty, and the
    limiter's wait at every vblank would be a gap every frame - which is what
    happened after the first slow moment (a game loading). So: silence until
    AUDIO_REFILL frames are there, then play on.
*/
#define AUDIO_REFILL 1024u /* ~23 ms */

static volatile uint32_t s_refill = 1u;

/* the half of s_dma the DMA has just finished with: from the ring, silence where it runs out */
static void audio_fill(uint32_t *dst)
{
    const uint32_t wr = s_wr;
    uint32_t rd = s_rd;
    uint32_t have = wr - rd;

    if (s_refill)
    {
        if (have < AUDIO_REFILL)
        {
            memset(dst, 0, AUDIO_HALF_FRAMES * sizeof(uint32_t));

            return;
        }

        s_refill = 0;
    }

    uint32_t n = (have < AUDIO_HALF_FRAMES) ? have : AUDIO_HALF_FRAMES;

    for (uint32_t i = 0; i < n; i++)
        dst[i] = AUDIO_PACED ? s_ring[(rd + i) & (AUDIO_RING_FRAMES - 1u)] : 0u;

    if (n < AUDIO_HALF_FRAMES)
    {
        /* dry: what there was, then silence, and wait for the ring to fill */
        memset(&dst[n], 0, (AUDIO_HALF_FRAMES - n) * sizeof(uint32_t));

        s_refill = 1u;

        if (s_ok)
            s_underruns++;
    }

    s_rd = rd + n;
}

void DMA0_DMA16_IRQHandler(void)
{
    if (DMA0->INT & (1u << AUDIO_DMA_CH))
    {
        DMA0->CINT = AUDIO_DMA_CH;

        /* past the middle of the buffer the first half is done; just wrapped, the second */
        const uint32_t citer = DMA0->TCD[AUDIO_DMA_CH].CITER_ELINKNO & DMA_CITER_ELINKNO_CITER_MASK;
        const uint32_t biter = DMA0->TCD[AUDIO_DMA_CH].BITER_ELINKNO & DMA_BITER_ELINKNO_BITER_MASK;

        audio_fill((citer <= (biter / 2u)) ? &s_dma[0] : &s_dma[AUDIO_HALF_FRAMES]);
    }

    __DSB();
}

/* the emulator's side (psx.h, PSXE_SOUND 2) */
/* every 32 samples: in OCRAM with the mixing, not in flash */
#define AUDIO_FAST __attribute__((section(".ramfunc.$SRAM_OC")))

void AUDIO_FAST audio_out_push(const int16_t *frames, uint32_t count);

void AUDIO_FAST psx_platform_audio_out(const int16_t *frames, uint32_t count)
{
    audio_out_push(frames, count);
}

/*
    The emulation never runs ahead of the sound. The frame limiter holds it to
    real time on average, but lets a few frames run fast to make up for slow
    ones (screen.c, psxe_screen_pace); what did not fit into the ring then was
    dropped - pieces of the sound missing, heard as tearing. Now the emulator
    waits until the output has played enough of the ring; the interrupt takes
    AUDIO_HALF_FRAMES (5.8 ms) at a time. Never longer than AUDIO_WAIT_MS, so
    that an output that stopped cannot stop the emulation.
*/
#define AUDIO_WAIT_MS 50u

void AUDIO_FAST audio_out_push(const int16_t *frames, uint32_t count)
{
#if AUDIO_PACED
    for (uint32_t waited = 0; s_ok && ((s_wr - s_rd) + count > AUDIO_RING_MAX) && (waited < AUDIO_WAIT_MS); waited++)
        vTaskDelay(1);
#endif

    uint32_t wr = s_wr;
    const uint32_t queued = wr - s_rd;
    const uint32_t room = (queued < AUDIO_RING_MAX) ? (AUDIO_RING_MAX - queued) : 0u;

    if (count > room)
        count = room;

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t f;

        memcpy(&f, &frames[2u * i], 4);
        s_ring[(wr + i) & (AUDIO_RING_FRAMES - 1u)] = f;
    }

    __DMB();
    s_wr = wr + count;
}

uint32_t audio_out_queued(void)
{
    return s_wr - s_rd;
}

uint32_t audio_out_underruns(void)
{
    const uint32_t n = s_underruns;

    s_underruns = 0;

    return n;
}

int32_t audio_out_ok(void)
{
    return s_ok;
}

int32_t audio_out_init(void)
{
    /* 12 MHz MCLK: audio PLL 24 MHz x 32 = 768 MHz, SAI1 root / 4 / 16 */
    const clock_audio_pll_config_t pll = {.loopDivider = 30, .postDivider = 1, .numerator = 200, .denominator = 100};

    CLOCK_InitAudioPll(&pll);
    CLOCK_SetMux(kCLOCK_Sai1Mux, 2);
    CLOCK_SetDiv(kCLOCK_Sai1PreDiv, 3);
    CLOCK_SetDiv(kCLOCK_Sai1Div, 15);

    /* the SAI drives MCLK out to the codec */
    IOMUXC_GPR->GPR1 |= IOMUXC_GPR_GPR1_SAI1_MCLK_DIR_MASK;

    CLOCK_EnableClock(kCLOCK_Iomuxc);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_09_SAI1_MCLK, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_13_SAI1_TX_DATA00, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_14_SAI1_TX_BCLK, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_15_SAI1_TX_SYNC, 1U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_09_SAI1_MCLK, 0x10B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_13_SAI1_TX_DATA00, 0x10B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_14_SAI1_TX_BCLK, 0x10B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_15_SAI1_TX_SYNC, 0x10B0U);

    /* SAI1 transmitter: I2S, 16 bit stereo, slave to the codec's clocks, a DMA
       request whenever its FIFO has room for half of it */
    sai_transceiver_t sai;

    SAI_Init(SAI1);
    SAI_GetClassicI2SConfig(&sai, kSAI_WordWidth16bits, kSAI_Stereo, 1U << 0);
    sai.masterSlave = kSAI_Slave;
    SAI_TxSetConfig(SAI1, &sai);

    /* eDMA channel 0: s_dma round and round into the SAI's data register,
       16 bits at a time, one FIFO watermark's worth per request */
    edma_config_t dma;

    EDMA_GetDefaultConfig(&dma);
    EDMA_Init(DMA0, &dma);
    DMAMUX_Init(DMAMUX);
    DMAMUX_SetSource(DMAMUX, AUDIO_DMA_CH, kDmaRequestMuxSai1Tx);
    DMAMUX_EnableChannel(DMAMUX, AUDIO_DMA_CH);

    memset(s_dma, 0, sizeof(s_dma));

    edma_transfer_config_t xfer;
    const uint32_t per_request = (uint32_t)sai.fifo.fifoWatermark * 2u;

    EDMA_PrepareTransfer(&xfer, s_dma, 2u, (void *)SAI_TxGetDataRegisterAddress(SAI1, 0), 2u, per_request,
                         sizeof(s_dma), kEDMA_MemoryToPeripheral);

    /* the TCD RAM holds whatever it powered up with, and without a next TCD the
       driver leaves DLAST_SGA alone: after the first pass the destination
       moved by that garbage and the DMA stopped on a bus error */
    EDMA_ResetChannel(DMA0, AUDIO_DMA_CH);
    EDMA_SetTransferConfig(DMA0, AUDIO_DMA_CH, &xfer, NULL);

    DMA0->TCD[AUDIO_DMA_CH].DLAST_SGA = 0;                                     /* the SAI register stays */
    DMA0->TCD[AUDIO_DMA_CH].SLAST = -(int32_t)sizeof(s_dma);                  /* back to the start */
    DMA0->TCD[AUDIO_DMA_CH].CSR = DMA_CSR_INTHALF_MASK | DMA_CSR_INTMAJOR_MASK; /* and never stop */

    NVIC_SetPriority(DMA0_DMA16_IRQn, 5);
    EnableIRQ(DMA0_DMA16_IRQn);
    EDMA_EnableChannelRequest(DMA0, AUDIO_DMA_CH);

    SAI_TxEnableDMA(SAI1, kSAI_FIFORequestDMAEnable, true);
    SAI_TxEnable(SAI1, true);

    /* the codec: I2S in -> DAC -> headphones, master at 44.1 kHz from its PLL */
    static wm8960_config_t codec = {
        .route = kWM8960_RoutePlayback,
        .leftInputSource = kWM8960_InputDifferentialMicInput3,
        .rightInputSource = kWM8960_InputDifferentialMicInput2,
        .playSource = kWM8960_PlaySourceDAC,
        .slaveAddress = WM8960_I2C_ADDR,
        .bus = kWM8960_BusI2S,
        .format = {.mclk_HZ = 12000000U, .sampleRate = kWM8960_AudioSampleRate44100Hz,
                   .bitWidth = kWM8960_AudioBitWidth16bit},
        .master_slave = true,
        .masterClock = {.sysclkSource = kWM8960_SysClkSourceInternalPLL, .sysclkFreq = 11289600U},
        .enableSpeaker = false,
    };

    /* LPI2C1 is shared with the touch panel, which set its clock: 60 MHz / (div + 1) */
    codec.i2cConfig.codecI2CInstance = 1U;
    codec.i2cConfig.codecI2CSourceClock =
        (CLOCK_GetFreq(kCLOCK_Usb1PllClk) / 8U) / (CLOCK_GetDiv(kCLOCK_Lpi2cDiv) + 1U);

    status_t st = WM8960_Init(&s_codec, &codec);

    if (st != kStatus_Success)
    {
        PRINTF("audio: WM8960 init failed (%d)\r\n", (int)st);

        return 1;
    }

    /* headphones at -6 dB (0x79 is 0 dB, 1 dB a step), the DAC at 0 dB (0xff):
       the SPU's main volume and the CD's volume are now applied as the hardware
       does (80h and 3FFFh = 100 %), twice what they were, and this keeps the
       loudness where it was */
    (void)WM8960_SetVolume(&s_codec, kWM8960_ModuleHP, 0x73U);
    (void)WM8960_SetVolume(&s_codec, kWM8960_ModuleDAC, 0xffU);

    s_ok = 1;

    PRINTF("audio: WM8960 on, 44.1 kHz 16 bit stereo to the headphone jack\r\n");

    return 0;
}
