/*
    Gamepad input from the ESP32 Bluetooth bridge.

    esp32-s3-bt-mod/ in this repository is a Zephyr application for a classic
    ESP32 (WROOM-32, the S3 has no Bluetooth Classic radio) that speaks
    Bluetooth Classic HID to a DualSense / DualSense Edge pad. It forwards the
    pad state as a small binary frame on a UART; this file is the other end of
    that link and drives the emulated controller from it.

    Wiring. LPUART3 is the board's otherwise unused "BT UART" and both of its
    pins are brought out to the Arduino header of the RT1050-EVKB (labelled
    D0 / D1 - check the silkscreen, the SoC pin names below are what matters):

        ESP32 GPIO17 (UART2 TX) -> GPIO_AD_B1_07, LPUART3_RXD
        ESP32 GPIO16 (UART2 RX) -> GPIO_AD_B1_06, LPUART3_TXD   (unused so far)
        ESP32 GND               -> board GND

    Only the receive direction carries data today. The transmit pin is muxed
    anyway, so a rumble or light bar channel can be added later without
    touching the board setup.

    Frame, 13 bytes, little endian, sent by the bridge whenever the pad state
    changes (and at least every 200 ms so the link can be seen as alive):

        0   0xa5            sync
        1   0x5a            sync
        2   buttons[7:0]    DualSense button bits, see the map below
        3   buttons[15:8]
        4   buttons[23:16]
        5   left stick x    0..255, 0x80 centred
        6   left stick y    0..255, 0x80 centred, larger is down
        7   right stick x
        8   right stick y
        9   L2              0 (released) .. 255 (fully pressed)
        10  R2
        11  sequence        increments once per frame
        12  checksum        XOR of bytes 2..11

    The bridge sends the DualSense bit layout as it comes off the wire and knows
    nothing about the PlayStation controller protocol; the mapping to the
    emulated pad lives here.
*/

#include "gamepad.h"

#include "board.h"
#include "fsl_lpuart.h"
#include "fsl_iomuxc.h"
#include "fsl_clock.h"

#include "input/sda.h"

#define GAMEPAD_UART LPUART3
#define GAMEPAD_UART_IRQ LPUART3_IRQn
#define GAMEPAD_UART_BAUDRATE 460800u

#define GAMEPAD_FRAME_LEN 13u
#define GAMEPAD_SYNC0 0xa5u
#define GAMEPAD_SYNC1 0x5au

/* the bit positions live in gamepad.h, the picker needs them too */
#define DS_DPAD_UP ((uint32_t)PSXE_DS_DPAD_UP)
#define DS_DPAD_RIGHT ((uint32_t)PSXE_DS_DPAD_RIGHT)
#define DS_DPAD_DOWN ((uint32_t)PSXE_DS_DPAD_DOWN)
#define DS_DPAD_LEFT ((uint32_t)PSXE_DS_DPAD_LEFT)
#define DS_SQUARE ((uint32_t)PSXE_DS_SQUARE)
#define DS_CROSS ((uint32_t)PSXE_DS_CROSS)
#define DS_CIRCLE ((uint32_t)PSXE_DS_CIRCLE)
#define DS_TRIANGLE ((uint32_t)PSXE_DS_TRIANGLE)
#define DS_L1 ((uint32_t)PSXE_DS_L1)
#define DS_R1 ((uint32_t)PSXE_DS_R1)
#define DS_L2 ((uint32_t)PSXE_DS_L2)
#define DS_R2 ((uint32_t)PSXE_DS_R2)
#define DS_CREATE ((uint32_t)PSXE_DS_CREATE)
#define DS_OPTIONS ((uint32_t)PSXE_DS_OPTIONS)
#define DS_L3 ((uint32_t)PSXE_DS_L3)
#define DS_R3 ((uint32_t)PSXE_DS_R3)
#define DS_PS ((uint32_t)PSXE_DS_PS)

/* How far the left stick has to leave the centre to count as a d-pad press */
#define GAMEPAD_STICK_THRESHOLD 48

typedef struct
{
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t l2, r2;
} gamepad_state_t;

/* written by the interrupt, read by psxe_gamepad_poll */
static volatile gamepad_state_t g_state;
static volatile uint32_t g_fresh;
static volatile uint32_t g_frames;
static volatile uint32_t g_errors;

/* interrupt private */
static uint8_t g_frame[GAMEPAD_FRAME_LEN];
static uint32_t g_index;

/* poll private */
static psx_pad_t *g_pad;
static int32_t g_slot;
static uint32_t g_applied; /* PSX switch mask currently pressed */
static uint32_t g_prev_ps;

/* Maps one DualSense bit to a PSX switch mask, or to 0 when it has no home */
static uint32_t gamepad_psx_mask(uint32_t bit)
{
    switch (bit)
    {
    case DS_DPAD_UP: return PSXI_SW_SDA_PAD_UP;
    case DS_DPAD_RIGHT: return PSXI_SW_SDA_PAD_RIGHT;
    case DS_DPAD_DOWN: return PSXI_SW_SDA_PAD_DOWN;
    case DS_DPAD_LEFT: return PSXI_SW_SDA_PAD_LEFT;
    case DS_SQUARE: return PSXI_SW_SDA_SQUARE;
    case DS_CROSS: return PSXI_SW_SDA_CROSS;
    case DS_CIRCLE: return PSXI_SW_SDA_CIRCLE;
    case DS_TRIANGLE: return PSXI_SW_SDA_TRIANGLE;
    case DS_L1: return PSXI_SW_SDA_L1;
    case DS_R1: return PSXI_SW_SDA_R1;
    case DS_L2: return PSXI_SW_SDA_L2;
    case DS_R2: return PSXI_SW_SDA_R2;
    case DS_CREATE: return PSXI_SW_SDA_SELECT;
    case DS_OPTIONS: return PSXI_SW_SDA_START;
    case DS_L3: return PSXI_SW_SDA_L3;
    case DS_R3: return PSXI_SW_SDA_R3;
    default: return 0;
    }
}

static uint32_t gamepad_translate(const gamepad_state_t *s)
{
    uint32_t mask = 0;

    for (uint32_t bit = 0; bit <= DS_R3; bit++)
    {
        if (s->buttons & (1u << bit))
            mask |= gamepad_psx_mask(bit);
    }

    /* A pad held by the sticks alone should still be able to walk around, so
       the left stick doubles as the d-pad. */
    const int32_t x = (int32_t)s->lx - 0x80;
    const int32_t y = (int32_t)s->ly - 0x80;

    if (x > GAMEPAD_STICK_THRESHOLD)
        mask |= PSXI_SW_SDA_PAD_RIGHT;
    else if (x < -GAMEPAD_STICK_THRESHOLD)
        mask |= PSXI_SW_SDA_PAD_LEFT;

    if (y > GAMEPAD_STICK_THRESHOLD)
        mask |= PSXI_SW_SDA_PAD_DOWN;
    else if (y < -GAMEPAD_STICK_THRESHOLD)
        mask |= PSXI_SW_SDA_PAD_UP;

    return mask;
}

/* One received byte, called from the interrupt */
static void gamepad_feed(uint8_t byte)
{
    if (g_index == 0u)
    {
        if (byte != GAMEPAD_SYNC0)
            return;
    }
    else if (g_index == 1u)
    {
        if (byte != GAMEPAD_SYNC1)
        {
            /* not a frame after all; the byte may itself start one */
            g_index = 0u;
            g_errors++;

            if (byte == GAMEPAD_SYNC0)
            {
                g_frame[0] = byte;
                g_index = 1u;
            }

            return;
        }
    }

    g_frame[g_index++] = byte;

    if (g_index < GAMEPAD_FRAME_LEN)
        return;

    g_index = 0u;

    uint8_t check = 0;

    for (uint32_t i = 2u; i < 12u; i++)
        check ^= g_frame[i];

    if (check != g_frame[12])
    {
        g_errors++;

        return;
    }

    g_state.buttons = (uint32_t)g_frame[2] | ((uint32_t)g_frame[3] << 8) |
                      ((uint32_t)g_frame[4] << 16);
    g_state.lx = g_frame[5];
    g_state.ly = g_frame[6];
    g_state.rx = g_frame[7];
    g_state.ry = g_frame[8];
    g_state.l2 = g_frame[9];
    g_state.r2 = g_frame[10];

    g_frames++;
    g_fresh = 1u;
}

void BOARD_BT_UART_IRQ_HANDLER(void)
{
    const uint32_t flags = LPUART_GetStatusFlags(GAMEPAD_UART);

    if (flags & (kLPUART_RxOverrunFlag | kLPUART_FramingErrorFlag | kLPUART_NoiseErrorFlag |
                 kLPUART_ParityErrorFlag))
    {
        LPUART_ClearStatusFlags(GAMEPAD_UART, kLPUART_RxOverrunFlag | kLPUART_FramingErrorFlag |
                                                  kLPUART_NoiseErrorFlag | kLPUART_ParityErrorFlag);

        /* whatever was in flight is lost, so give up on the current frame */
        g_index = 0u;
        g_errors++;
    }

    while (LPUART_GetStatusFlags(GAMEPAD_UART) & kLPUART_RxDataRegFullFlag)
        gamepad_feed(LPUART_ReadByte(GAMEPAD_UART));

    __DSB();
}

void psxe_gamepad_bind(psx_pad_t *pad, int32_t slot)
{
    g_pad = pad;
    g_slot = slot;

    /* nothing is held down from the picker's point of view */
    g_applied = 0u;
}

uint32_t psxe_gamepad_raw_buttons(void)
{
    return g_state.buttons;
}

void psxe_gamepad_init(void)
{
    g_pad = 0;
    g_slot = 0;
    g_applied = 0u;
    g_index = 0u;
    g_fresh = 0u;

    /* RX gets a pull-up so an unconnected bridge leaves the line idle high
       instead of generating noise interrupts. */
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_06_LPUART3_TXD, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_07_LPUART3_RXD, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_06_LPUART3_TXD, 0x10B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_07_LPUART3_RXD, 0xB0B0U);

    /* The board configuration already leaves this gate on, but a peripheral
       should not depend on that. */
    CLOCK_EnableClock(kCLOCK_Lpuart3);

    lpuart_config_t config;

    LPUART_GetDefaultConfig(&config);

    config.baudRate_Bps = GAMEPAD_UART_BAUDRATE;
    config.enableTx = false;
    config.enableRx = true;

    LPUART_Init(GAMEPAD_UART, &config, BOARD_BT_UART_CLK_FREQ);

    LPUART_EnableInterrupts(GAMEPAD_UART, kLPUART_RxDataRegFullInterruptEnable |
                                              kLPUART_RxOverrunInterruptEnable);

    NVIC_SetPriority(GAMEPAD_UART_IRQ, 6);
    EnableIRQ(GAMEPAD_UART_IRQ);
}

void psxe_gamepad_poll(void)
{
    if (!g_fresh || !g_pad)
        return;

    gamepad_state_t s;

    const uint32_t masked = DisableGlobalIRQ();

    s = *(const gamepad_state_t *)&g_state;
    g_fresh = 0u;

    EnableGlobalIRQ(masked);

    const uint32_t mask = gamepad_translate(&s);
    const uint32_t changed = mask ^ g_applied;

    if (changed & mask)
        psx_pad_button_press(g_pad, g_slot, changed & mask);

    if (changed & ~mask)
        psx_pad_button_release(g_pad, g_slot, changed & ~mask);

    g_applied = mask;

    /* PS toggles the analog mode of the emulated pad. It has to go in on its
       own: the controller reads that particular value as a mode switch. */
    const uint32_t ps = (s.buttons >> DS_PS) & 1u;

    if (ps && !g_prev_ps)
        psx_pad_button_press(g_pad, g_slot, PSXI_SW_SDA_ANALOG);

    g_prev_ps = ps;
}

int32_t psxe_gamepad_is_connected(void)
{
    return (g_frames != 0u) ? 1 : 0;
}

void psxe_gamepad_get_stats(uint32_t *frames, uint32_t *errors)
{
    if (frames)
        *frames = g_frames;

    if (errors)
        *errors = g_errors;
}
