/*
    Core clock above the 600 MHz the i.MX RT1050 is specified for.

    The ARM PLL makes 12 MHz x DIV_SELECT (54..108: 648..1296 MHz). The stock
    setup runs it at 1200 MHz and divides by two; here it runs at the core clock
    itself. The core voltage (VDD_SOC, from the on-chip DCDC) goes up first, by
    the rule Teensy 4 uses for the same core: 1.25 V plus 25 mV for every 28 MHz
    above 600 - 1.425 V at 816 MHz, against the stock 1.275 V. That is outside
    NXP's ratings, as is the clock: more heat (the EVKB has no heat sink), and
    the part may not last as long. BOARD_TempCelsius tells how hot it runs.

    What else changes: IPG stays at a quarter of the core (204 MHz at 816, also
    above its 150 MHz rating, as on Teensy), PERCLK becomes a third of that
    (68 MHz). SDRAM (SEMC), FlexSPI, LCD pixel, UART, SD card and I2C clocks come
    from other PLLs and stay as they are. Everything that times itself reads
    SystemCoreClock (FreeRTOS, the frame limiter) or the IPG clock from the CCM
    (Ethernet), so it follows.
*/
#include "overclock.h"

#include "fsl_common.h"
#include "fsl_clock.h"

uint32_t BOARD_SetCoreClock(uint32_t mhz)
{
    if ((mhz <= 600u) || (mhz > 1296u))
        return 1275u; /* the stock setup (DCDC_REG3 TRG 0x13) */

    const uint32_t div = mhz / 12u;
    const uint32_t mv = (PSXE_CPU_MV >= 1275u) ? (uint32_t)PSXE_CPU_MV : (1250u + (((mhz - 600u) / 28u) * 25u));
    uint32_t trg = (mv - 800u) / 25u; /* 0.8 V + 25 mV per step */

    /* the DCDC goes no higher than 1.575 V: above 984 MHz the rule wants more */
    if (trg > 31u)
        trg = 31u;

    /* the voltage first: the core must be able to take the clock before it gets it */
    DCDC->REG3 = (DCDC->REG3 & ~DCDC_REG3_TRG_MASK) | DCDC_REG3_TRG(trg);

    while (0u == (DCDC->REG0 & DCDC_REG0_STS_DC_OK_MASK))
    {
    }

    /* the core on the 24 MHz oscillator while the PLL changes */
    CLOCK_SetMux(kCLOCK_PeriphClk2Mux, 1);
    CLOCK_SetMux(kCLOCK_PeriphMux, 1);

    const clock_arm_pll_config_t pll = {.loopDivider = div, .src = 0};

    CLOCK_InitArmPll(&pll);

    CLOCK_SetDiv(kCLOCK_IpgDiv, 3);    /* IPG = core / 4 */
    CLOCK_SetDiv(kCLOCK_PerclkDiv, 2); /* PERCLK = IPG / 3 */
    CLOCK_SetDiv(kCLOCK_ArmDiv, 0);    /* core = PLL / 1 */
    CLOCK_SetDiv(kCLOCK_AhbDiv, 0);

    /* back onto the PLL */
    CLOCK_SetMux(kCLOCK_PrePeriphMux, 3);
    CLOCK_SetMux(kCLOCK_PeriphMux, 0);
    CLOCK_SetMux(kCLOCK_PeriphClk2Mux, 0);

    SystemCoreClock = div * 12000000u;

    return 800u + (trg * 25u);
}

/* the hottest BOARD_TempCelsius has seen, for reading through the probe */
volatile int32_t __attribute__((used)) g_core_temp_max = -1000;

void BOARD_TempStart(void)
{
    /* the fuses hold the sensor's calibration */
    CLOCK_EnableClock(kCLOCK_Ocotp);

    /* a new measurement every 4096 cycles of the 32 kHz clock (1/8 s) */
    TEMPMON->TEMPSENSE1 = TEMPMON_TEMPSENSE1_MEASURE_FREQ(0x1000u);
    TEMPMON->TEMPSENSE0_CLR = TEMPMON_TEMPSENSE0_POWER_DOWN_MASK;
    TEMPMON->TEMPSENSE0_SET = TEMPMON_TEMPSENSE0_MEASURE_TEMP_MASK;
}

int32_t BOARD_TempCelsius(void)
{
    const uint32_t s = TEMPMON->TEMPSENSE0;

    if (0u == (s & TEMPMON_TEMPSENSE0_FINISHED_MASK))
        return -1000;

    /* the fused calibration: the count at 25 C and at a hot temperature */
    const uint32_t ana1 = OCOTP->ANA1;
    const int32_t hot_c = (int32_t)(ana1 & 0xffu);
    const int32_t hot_count = (int32_t)((ana1 >> 8) & 0xfffu);
    const int32_t room_count = (int32_t)(ana1 >> 20);
    const int32_t count = (int32_t)((s & TEMPMON_TEMPSENSE0_TEMP_CNT_MASK) >> TEMPMON_TEMPSENSE0_TEMP_CNT_SHIFT);

    if (room_count == hot_count)
        return -1000;

    const int32_t c = hot_c - (((count - hot_count) * (hot_c - 25)) / (room_count - hot_count));

    if (c > g_core_temp_max)
        g_core_temp_max = c;

    return c;
}
