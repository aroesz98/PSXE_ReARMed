/*
    GT911 capacitive touch panel.

    Wiring is fixed by the board: LPI2C1 on GPIO_AD_B1_00 (SCL) and
    GPIO_AD_B1_01 (SDA), reset on GPIO_AD_B0_02 (GPIO1_IO02) and the interrupt
    line on GPIO_AD_B0_11 (GPIO1_IO11). The interrupt pin doubles as the address
    select during reset, which is why the controller needs to drive it - the
    driver asks for that through intPinFunc.

    Only polling is used here: the picker reads the panel once per frame, and the
    emulator does not use touch at all.
*/

#include "touch.h"

#include "board.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"
#include "fsl_lpi2c.h"
#include "fsl_clock.h"
#include "fsl_common.h"
#include "fsl_debug_console.h"

#include "fsl_gt911.h"

/* The panel fitted to this board; the controller is asked for its own
   resolution at init and the readings are scaled into these. */
#define UI_TOUCH_PANEL_X 480
#define UI_TOUCH_PANEL_Y 272
#define UI_TOUCH_FALLBACK_X 480
#define UI_TOUCH_FALLBACK_Y 272

#define TOUCH_I2C BOARD_TOUCH_I2C_BASEADDR

/* USB1 PLL (480 MHz) / 8 / (divider + 1) */
#define TOUCH_LPI2C_CLOCK_SOURCE_SELECT (0U)
#define TOUCH_LPI2C_CLOCK_SOURCE_DIVIDER (5U)
#define TOUCH_I2C_CLOCK_FREQ ((CLOCK_GetFreq(kCLOCK_Usb1PllClk) / 8U) / (TOUCH_LPI2C_CLOCK_SOURCE_DIVIDER + 1U))

static gt911_handle_t s_touch;
static int s_touch_ok;
static int s_res_x = UI_TOUCH_FALLBACK_X;
static int s_res_y = UI_TOUCH_FALLBACK_Y;

static void touch_delay_ms(uint32_t ms)
{
    SDK_DelayAtLeastUs(ms * 1000U, SystemCoreClock);
}

static void touch_pull_reset(bool pull_up)
{
    GPIO_PinWrite(BOARD_TOUCH_RST_GPIO, BOARD_TOUCH_RST_PIN, pull_up ? 1U : 0U);
}

static void touch_config_int_pin(gt911_int_pin_mode_t mode)
{
    if (mode == kGT911_IntPinInput)
    {
        BOARD_TOUCH_INT_GPIO->GDIR &= ~(1UL << BOARD_TOUCH_INT_PIN);
    }
    else
    {
        GPIO_PinWrite(BOARD_TOUCH_INT_GPIO, BOARD_TOUCH_INT_PIN,
                      (mode == kGT911_IntPinPullDown) ? 0U : 1U);

        BOARD_TOUCH_INT_GPIO->GDIR |= (1UL << BOARD_TOUCH_INT_PIN);
    }
}

static const gt911_config_t s_touch_config = {
    .I2C_SendFunc = BOARD_Touch_I2C_Send,
    .I2C_ReceiveFunc = BOARD_Touch_I2C_Receive,
    .pullResetPinFunc = touch_pull_reset,
    .intPinFunc = touch_config_int_pin,
    .timeDelayMsFunc = touch_delay_ms,
    .touchPointNum = 1,
    .i2cAddrMode = kGT911_I2cAddrMode0,
    .intTrigMode = kGT911_IntRisingEdge,
};

int32_t psxe_touch_init(void)
{
    const gpio_pin_config_t pin_config = {
        .direction = kGPIO_DigitalOutput,
        .outputLogic = 0,
        .interruptMode = kGPIO_NoIntmode,
    };

    /* I2C1 pins: open drain with a pull-up, as the bus needs */
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_00_LPI2C1_SCL, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_01_LPI2C1_SDA, 1U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_00_LPI2C1_SCL, 0xD8B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_01_LPI2C1_SDA, 0xD8B0U);

    /* reset and interrupt lines start as outputs, the driver flips INT around */
    GPIO_PinInit(BOARD_TOUCH_INT_GPIO, BOARD_TOUCH_INT_PIN, &pin_config);
    GPIO_PinInit(BOARD_TOUCH_RST_GPIO, BOARD_TOUCH_RST_PIN, &pin_config);

    CLOCK_SetMux(kCLOCK_Lpi2cMux, TOUCH_LPI2C_CLOCK_SOURCE_SELECT);
    CLOCK_SetDiv(kCLOCK_Lpi2cDiv, TOUCH_LPI2C_CLOCK_SOURCE_DIVIDER);

    BOARD_LPI2C_Init(TOUCH_I2C, TOUCH_I2C_CLOCK_FREQ);

    if (kStatus_Success != GT911_Init(&s_touch, &s_touch_config))
    {
        PRINTF("touch: GT911 did not answer on I2C1\r\n");

        s_touch_ok = 0;

        return 0;
    }

    GT911_GetResolution(&s_touch, &s_res_x, &s_res_y);

    if ((s_res_x <= 0) || (s_res_y <= 0))
    {
        s_res_x = UI_TOUCH_FALLBACK_X;
        s_res_y = UI_TOUCH_FALLBACK_Y;
    }

    PRINTF("touch: GT911 ready, %dx%d\r\n", s_res_x, s_res_y);

    s_touch_ok = 1;

    return 1;
}

/*
    The controller only refreshes its buffer when it has something new, and the
    driver reports that as a plain failure - the same value it uses for an I2C
    error. Polling faster than the panel updates therefore looks like a finger
    tapping the screen over and over, which is no good for a touch UI.

    So the contact state is held here: a successful read means down, an explicit
    "not touched" means up, and anything else means "nothing new, keep what we
    had". A long run without a single good read falls back to released, so a
    broken bus cannot leave a stuck press behind.
*/
#define TOUCH_STALE_LIMIT 40

static int32_t s_down;
static int32_t s_last_x;
static int32_t s_last_y;
static int32_t s_stale;

int32_t psxe_touch_read(int32_t *x, int32_t *y)
{
    if (!s_touch_ok)
        return 0;

    int raw_x = 0;
    int raw_y = 0;

    const status_t status = GT911_GetSingleTouch(&s_touch, &raw_x, &raw_y);

    if (kStatus_Success == status)
    {
        s_last_x = (raw_x * UI_TOUCH_PANEL_X) / s_res_x;
        s_last_y = (raw_y * UI_TOUCH_PANEL_Y) / s_res_y;
        s_down = 1;
        s_stale = 0;
    }
    else if ((status_t)kStatus_TOUCHPANEL_NotTouched == status)
    {
        s_down = 0;
        s_stale = 0;
    }
    else if (++s_stale > TOUCH_STALE_LIMIT)
    {
        s_down = 0;
    }

    if (x)
        *x = s_last_x;

    if (y)
        *y = s_last_y;

    return s_down;
}
