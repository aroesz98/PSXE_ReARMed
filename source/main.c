/*
 * Copyright 2020 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "gpu_switch.h"
#include "enet_link.h"
#include "psxe_link.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* FreeRTOS kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* FATFS includes. */
#include "ff.h"

#include "sdcard.h"
#include "display.h"
#include "display_support.h"
#include "fsl_debug_console.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "overclock.h"
#include "audio_out.h"
#include "sound_switch.h"
#include "board.h"
#include "fsl_gpio.h"
#include "MIMXRT1052.h"

/* DWT cycle counter for performance monitoring */
#include "core_cm7.h"

/* PSX Emulator includes */
#include "psx.h"
#include "prof.h"
#include "input/sda.h"
#include "gamepad.h"
#include "menu.h"
#include "input/guncon.h"
#include "dev/cdrom/cdrom.h"
#include "dev/timer.h"
#include "cpu.h"
#include "frontend/screen.h"
#include "log.h"
#include "dev/gpu.h"
#include "dev/gpu_test.h"

/* Standard type includes */
#include <stdint.h>
/*******************************************************************************
 * Definitions
 ******************************************************************************/

/* Simple PSX configuration structure for MCU */
typedef struct
{
    const char *bios_path; /* Path to BIOS file on SD card */
    const char *cd_path;   /* Path to CD image on SD card */
    const char *exe_path;  /* Path to PSX executable */
    int scale;             /* Display scale factor */
    int log_level;         /* Logging level */
} psx_config_t;

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
/* ---------------------------------------------------------------------------
   Memory micro benchmark: tells us what the memory system actually delivers
   for the access patterns the GPU rasterizer uses (VRAM lives in SDRAM).
   --------------------------------------------------------------------------- */
/* Set to 1 to print the memory benchmark at boot */
#define PSX_MEM_BENCH 0

/* The memory cards: files in the root of the SD card. Slot 2 costs another
   128 KB of heap and is off; most games save to slot 1. */
#ifndef PSXE_MCD1_PATH
#define PSXE_MCD1_PATH "/memcard1.mcd"
#endif

#ifndef PSXE_MCD2
#define PSXE_MCD2 0
#endif

#ifndef PSXE_MCD2_PATH
#define PSXE_MCD2_PATH "/memcard2.mcd"
#endif

/* The stock NXP DCD programs the SDRAM (and the SEMC) for burst length 1, so a
   single 32 byte cache line refill turns into 16 separate SDRAM accesses.
   VRAM and PSX RAM live in SDRAM, so this directly limits the rasterizer. */
static void BOARD_SDRAM_SetBurstLen8(void)
{
    while (0U == (SEMC->STS0 & SEMC_STS0_IDLE_MASK))
    {
    }

    /* SDRAM mode register: CAS latency 3, burst length 8 */
    SEMC->IPTXDAT = 0x33U;
    SEMC->IPCR0 = 0x80000000U;
    SEMC->IPCR1 = 2U;
    SEMC->IPCMD = 0xA55A000AU; /* MODESET */

    while (0U == (SEMC->INTR & SEMC_INTR_IPCMDDONE_MASK))
    {
    }

    SEMC->INTR = SEMC_INTR_IPCMDDONE_MASK;

    /* And let the controller issue 8 beat bursts */
    SEMC->SDRAMCR0 = (SEMC->SDRAMCR0 & ~SEMC_SDRAMCR0_BL_MASK) | SEMC_SDRAMCR0_BL(3);

    __DSB();
    __ISB();
}

/* Core cycles for one data cache line fill from SDRAM, averaged over lines that
   cannot be in the cache: every emulator hot path that leaves the tightly
   coupled memories is bound by this number. About 100 with 8 beat bursts, many
   times that without. Needs the DWT cycle counter running. */
static uint32_t BOARD_SDRAM_LineFillCycles(void)
{
    /* far away from anything that has been touched this early */
    const volatile uint8_t *p = (const volatile uint8_t *)0x81400000u;
    uint32_t sum = 0;

    const uint32_t t0 = DWT->CYCCNT;

    for (uint32_t i = 0; i < 1024u; i++)
        sum += p[i * 64u];

    const uint32_t cyc = DWT->CYCCNT - t0;

    (void)sum;

    return cyc / 1024u;
}

__attribute__((unused)) static void psx_mem_benchmark(void *vram, const char *tag)
{
    volatile uint16_t *p16 = (volatile uint16_t *)vram;
    volatile uint32_t *p32 = (volatile uint32_t *)vram;
    uint32_t t0, cyc, i, acc = 0;

    const uint32_t words = 64 * 1024; /* 256 KB */

    /* cost of a DWT cycle counter read (the profiler uses two per round) */
    t0 = DWT->CYCCNT;
    for (i = 0; i < 1000; i++)
        acc += DWT->CYCCNT;
    cyc = DWT->CYCCNT - t0;
    PRINTF("[%s] BENCH DWT read: %u cyc each\r\n", tag, cyc / 1000);

    /* sequential 32-bit write */
    t0 = DWT->CYCCNT;
    for (i = 0; i < words; i++)
        p32[i] = i;
    cyc = DWT->CYCCNT - t0;
    PRINTF("[%s] BENCH seq write 256KB: %u cycles (%u MB/s, %u cyc/word)\r\n", tag,
           cyc, (unsigned)((uint64_t)256 * 600000000u / 1024u / cyc), cyc / words);

    /* sequential 32-bit read */
    t0 = DWT->CYCCNT;
    for (i = 0; i < words; i++)
        acc += p32[i];
    cyc = DWT->CYCCNT - t0;
    PRINTF("[%s] BENCH seq read  256KB: %u cycles (%u MB/s, %u cyc/word)\r\n", tag,
           cyc, (unsigned)((uint64_t)256 * 600000000u / 1024u / cyc), cyc / words);

    /* 16-bit reads with a 2048 byte stride: one VRAM line per access */
    t0 = DWT->CYCCNT;
    for (i = 0; i < 16384; i++)
        acc += p16[(i * 1024) & 0x7ffff];
    cyc = DWT->CYCCNT - t0;
    PRINTF("[%s] BENCH strided read (VRAM lines): %u cyc/access\r\n", tag, cyc / 16384);

    /* 16-bit writes with a 2048 byte stride */
    t0 = DWT->CYCCNT;
    for (i = 0; i < 16384; i++)
        p16[(i * 1024) & 0x7ffff] = (uint16_t)i;
    cyc = DWT->CYCCNT - t0;
    PRINTF("[%s] BENCH strided write (VRAM lines): %u cyc/access\r\n", tag, cyc / 16384);

    /* sequential 16-bit span write (rasterizer fill pattern) */
    t0 = DWT->CYCCNT;
    for (i = 0; i < 131072; i++)
        p16[i] = (uint16_t)i;
    cyc = DWT->CYCCNT - t0;
    PRINTF("[%s] BENCH span write 16-bit: %u cyc/pixel\r\n", tag, cyc / 131072);

    /* sequential 16-bit read with software prefetch one line ahead */
    t0 = DWT->CYCCNT;
    for (i = 0; i < 131072; i++)
    {
        if ((i & 15u) == 0u)
            __builtin_prefetch((const void *)&p16[i + 16]);

        acc += p16[i];
    }
    cyc = DWT->CYCCNT - t0;
    PRINTF("[%s] BENCH span read 16-bit + PLD: %u cyc/texel\r\n", tag, cyc / 131072);

    /* strided read with prefetch of the following line */
    t0 = DWT->CYCCNT;
    for (i = 0; i < 16384; i++)
    {
        __builtin_prefetch((const void *)&p16[((i + 1) * 1024) & 0x7ffff]);

        acc += p16[(i * 1024) & 0x7ffff];
    }
    cyc = DWT->CYCCNT - t0;
    PRINTF("[%s] BENCH strided read + PLD: %u cyc/access\r\n", tag, cyc / 16384);

    /* sequential 16-bit read (texel fetch pattern along one texture row) */
    t0 = DWT->CYCCNT;
    for (i = 0; i < 131072; i++)
        acc += p16[i];
    cyc = DWT->CYCCNT - t0;
    PRINTF("[%s] BENCH span read 16-bit: %u cyc/texel (acc=%u)\r\n", tag, cyc / 131072, (unsigned)acc);
}

static void psx_emulator_task(void *pvParameters);

/* Empty audio callback for future use */
void audio_update(void *ud, uint8_t *buf, int size)
{
    /* Audio implementation will be added later */
    memset(buf, 0, size);
}

/*******************************************************************************
 * Variables
 ******************************************************************************/

/* FreeRTOS heap placed in DTCM: every task stack (including the emulator
   task) then lives in tightly coupled memory instead of ITCM, which is
   reserved for hot code. */
uint8_t __attribute__((section(".bss.$SRAM_DTC"), aligned(8))) ucHeap[configTOTAL_HEAP_SIZE];

/* PSX Emulator global variables */
static psx_t *g_psx = NULL;

/* how far the start got, for the probe (see the psxe_boot_stage calls below) */
volatile int g_boot_stage;
static psxe_screen_t *g_screen = NULL;
static bool g_psxInitialized = false;

/* Default PSX configuration for MCU */
static psx_config_t g_psxConfig = {
    .bios_path = "/bios/SCPH-1001.BIN",                 /* BIOS file on SD card root */
    .cd_path = "/Final Fantasy VII (USA) (Disc 1).cue", /* CD image path */
    .exe_path = NULL,                                   /* No executable initially */
    .scale = 1,                                         /* Default scale */
    .log_level = 4                                      /* Error level logging only (reduces verbosity) */
};

/*******************************************************************************
 * Code
 ******************************************************************************/

int main(void)
{
    /* Init board hardware. */
    BOARD_ConfigMPU();
    BOARD_InitPins();
    BOARD_BootClockRUN();
    BOARD_InitDebugConsole();

    CLOCK_InitSysPfd(kCLOCK_Pfd2, 23);
    /* Set semc clock to 176 MHz (528 * 18 / 27 / 2) */
    CLOCK_SetMux(kCLOCK_SemcMux, 1);
    CLOCK_SetDiv(kCLOCK_SemcDiv, 1);
//    CLOCK_SetDiv(kCLOCK_Usdhc1Div, 2);
//    CLOCK_SetDiv(kCLOCK_Usdhc2Div, 2);

    /* the core clock above the stock 600 MHz, and the voltage for it (overclock.c) */
    const uint32_t core_mv = BOARD_SetCoreClock(PSXE_CPU_MHZ);

    BOARD_TempStart();

    PRINTF("PSXE MCU Emulator Starting...\r\n");

    /* DWT cycle counter, used by the benchmarks and the profiler */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* Burst length 8 makes SDRAM line fills (VRAM, PSX RAM) much cheaper */
#if PSX_MEM_BENCH
    psx_mem_benchmark((void *)0x81000000u, "BL1");
#endif
    BOARD_SDRAM_SetBurstLen8();
#if PSX_MEM_BENCH
    psx_mem_benchmark((void *)0x81000000u, "BL8");
#endif

    /* Everything outside the tightly coupled memories runs at the speed of this
       number, so it is worth one line in every boot log: if a game is ever
       unaccountably slow, this says whether the memory was. */
    {
        uint32_t fill = BOARD_SDRAM_LineFillCycles();

        if (fill > 400u)
        {
            /* the mode register write did not take: once more */
            BOARD_SDRAM_SetBurstLen8();

            fill = BOARD_SDRAM_LineFillCycles();
        }

        PRINTF("SDRAM: %u core cycles per cache line fill, SDRAMCR0=%08x%s\r\n",
               (unsigned int)fill, (unsigned int)SEMC->SDRAMCR0, (fill > 400u) ? " - SLOW" : "");
    }
    PRINTF("Initial Core Clock: %u Hz (%u MHz) at %u mV, IPG %u MHz, %d C\r\n", (unsigned int)SystemCoreClock,
           (unsigned int)(SystemCoreClock / 1000000), (unsigned int)core_mv,
           (unsigned int)(CLOCK_GetFreq(kCLOCK_IpgClk) / 1000000u), (int)BOARD_TempCelsius());

/* Display architecture information */
#ifdef __arm__
    PRINTF("PSXE Architecture: 32-bit (ARM Cortex-M7)\r\n");
#else
    PRINTF("PSXE Architecture: Unknown\r\n");
#endif

    PRINTF("Pointer size: %u bytes (%s)\r\n", (unsigned int)sizeof(void *),
           sizeof(void *) == 8 ? "64-bit" : sizeof(void *) == 4 ? "32-bit"
                                                                : "unknown");

    /* Create PSX emulator task */
    PRINTF("Creating PSX emulator task...\r\n");
    if (xTaskCreate(psx_emulator_task, "psx_emulator_task", 12000, NULL, configMAX_PRIORITIES - 2, NULL) != pdPASS)
    {
        PRINTF("PSX emulator task creation failed!.\r\n");
        while (1)
            ;
    }
    PRINTF("PSX emulator task created successfully\r\n");

    PRINTF("Starting FreeRTOS scheduler...\r\n");
    vTaskStartScheduler();

    /* Should never reach here */
    PRINTF("ERROR: FreeRTOS scheduler returned!\r\n");
    for (;;)
        ;
}

static void psx_emulator_task(void *pvParameters)
{
    PRINTF("=== PSX Emulator task starting ===\r\n");
    PRINTF("Task priority: %d\r\n", (int)uxTaskPriorityGet(NULL));
    PRINTF("Available heap: %d bytes\r\n", (int)xPortGetFreeHeapSize());

    /* Mount SD card for PSX emulator */
    PRINTF("Mounting SD card...\r\n");
    if (0 != MOUNT_SDCard())
    {
        PRINTF("SD card mount error. Demo stopped!");
        configASSERT(0);
    }
    PRINTF("SD card mounted successfully\r\n");

    /* The controller link comes up before anything else: the picker below is
       driven by it, and the emulated pad is attached to it later. */
    psxe_gamepad_init();

    /* The panel has to be alive for the picker to draw on it; psxe_screen_init
       calls this again later, which is harmless. */
    DEMO_InitLcd();

    {
        static char chosen_path[192];

        g_boot_stage = 1; /* the picker */

        if (psxe_menu_pick(chosen_path, sizeof(chosen_path)))
            g_psxConfig.cd_path = chosen_path;
        else
            PRINTF("menu: no disc images found, keeping %s\r\n", g_psxConfig.cd_path);

        g_boot_stage = 2; /* a game chosen */
    }

    /* Set PSX emulator log level to reduce verbosity */
    log_set_level(g_psxConfig.log_level);
    PRINTF("PSX log level set to: %d (ERROR and FATAL only)\r\n", g_psxConfig.log_level);

#if PSXE_GPU_REMOTE
    /* the Ethernet link to the GPU board; the game starts even without it */
    {
        static const uint8_t mac_cpu[6] = PSXE_LINK_MAC_CPU;

        if (enet_link_init(mac_cpu) != 0)
            PRINTF("GPU link: Ethernet init failed, nothing will be drawn\r\n");
    }
#endif

    /* Initialize PSX emulator */
    g_boot_stage = 3; /* the console */
    PRINTF("Creating PSX emulator instance...\r\n");
    g_psx = psx_create();
    if (!g_psx)
    {
        PRINTF("PSX creation failed!\r\n");
        configASSERT(0);
    }
    PRINTF("PSX emulator instance created\r\n");

    /* Initialize PSX with BIOS - using configuration */
    PRINTF("Initializing PSX core (BIOS: %s)...\r\n", g_psxConfig.bios_path ? g_psxConfig.bios_path : "NULL");
    int initResult = psx_init(g_psx, g_psxConfig.bios_path, NULL); /* bios_path, exp_path */
    if (initResult != 0)
    {
        PRINTF("PSX initialization failed with code: %d\r\n", initResult);
        PRINTF("Note: BIOS file '%s' may not be found on SD card\r\n",
               g_psxConfig.bios_path ? g_psxConfig.bios_path : "NULL");
        /* Continue anyway for testing */
    }
    else
    {
        PRINTF("PSX core initialized successfully\r\n");
    }

    /* Get CDROM handle */
    g_boot_stage = 4; /* the disc */
    psx_cdrom_t *cdrom = psx_get_cdrom(g_psx);
    if (cdrom)
    {
        /* To-do: Set CDROM firmware version and region based on configuration */
        if (g_psxConfig.cd_path)
        {
            PRINTF("Loading CD image: %s\r\n", g_psxConfig.cd_path);
            int cd_result = psx_cdrom_open(cdrom, g_psxConfig.cd_path);
            if (cd_result)
            {
                PRINTF("CD image loaded successfully\r\n");
            }
            else
            {
                PRINTF("CD image loading failed\r\n");
            }
        }
        PRINTF("CDROM ready\r\n");
    }

    /* Initialize screen/display */
    g_boot_stage = 5; /* the screen */
    PRINTF("Creating PSX screen/display system...\r\n");
    g_screen = psxe_screen_create();
    if (!g_screen)
    {
        PRINTF("Screen creation failed!\r\n");
        configASSERT(0);
    }
    PRINTF("Screen created successfully\r\n");

    PRINTF("Initializing screen with PSX instance...\r\n");
    psxe_screen_init(g_screen, g_psx);
    psxe_screen_set_scale(g_screen, g_psxConfig.scale); /* Use configured scale */
    psxe_screen_reload(g_screen);

    /* Check if screen is open */
    if (psxe_screen_is_open(g_screen))
    {
        PRINTF("Screen is open and ready\r\n");
    }
    else
    {
        PRINTF("WARNING: Screen is not open!\r\n");
    }

    /* Clear the framebuffer initially (PSX will take over rendering) */
    if (g_screen->framebuffer && g_screen->backbuffer)
    {
        /* Clear to black - PSX GPU will render actual content */
        memset(g_screen->framebuffer, 0x00, LCD_WIDTH * LCD_HEIGHT * 2);
        memset(g_screen->backbuffer, 0x00, LCD_WIDTH * LCD_HEIGHT * 2);
        PRINTF("Framebuffer cleared for PSX rendering\r\n");
    }
    else
    {
        PRINTF("WARNING: Framebuffer or backbuffer is NULL!\r\n");
    }

    PRINTF("Display system initialized\r\n");

    /* Set up GPU event callbacks */
    psx_gpu_t *gpu = psx_get_gpu(g_psx);
    if (gpu)
    {
        psx_gpu_set_event_callback(gpu, GPU_EVENT_DMODE, psxe_gpu_dmode_event_cb);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_VBLANK, psxe_gpu_vblank_event_cb);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_HBLANK, psxe_gpu_hblank_event_cb);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_VBLANK_END, psxe_gpu_vblank_end_event_cb);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_HBLANK_END, psxe_gpu_hblank_end_event_cb);
        /* Add other GPU event callbacks as needed */
        psx_gpu_set_udata(gpu, 0, g_screen);
        psx_gpu_set_udata(gpu, 1, g_psx->timer);
        PRINTF("GPU callbacks configured\r\n");
    }

    /* Initialize input system */
    psx_input_t *input = psx_input_create();
    if (input)
    {
        psx_input_init(input);

        psxi_sda_t *controller = psxi_sda_create();
        if (controller)
        {
            psxi_sda_init(controller, SDA_MODEL_DIGITAL);
            psxi_sda_init_input(controller, input);
        }

        psx_pad_attach_joy(g_psx->pad, 0, input);

        /* the link is already up, point it at the emulated controller */
        psxe_gamepad_bind(g_psx->pad, 0);

        PRINTF("Input system initialized\r\n");
    }

    g_boot_stage = 6; /* the memory card */

    /* Memory cards: files on the SD card, 128 KB raw images (the .mcd / .mcr
       format other emulators use). A missing file becomes a new, formatted
       card; what a game saves reaches the file half a second after it stops
       writing (psx_pad_tick_mcd in the loop below). */
    {
        const int32_t r = psx_pad_attach_mcd(g_psx->pad, 0, PSXE_MCD1_PATH);

        if (r)
            PRINTF("Memory card 1 (%s) not available: error %d\r\n", PSXE_MCD1_PATH, (int)r);

#if PSXE_MCD2
        if (psx_pad_attach_mcd(g_psx->pad, 1, PSXE_MCD2_PATH))
            PRINTF("Memory card 2 (%s) not available\r\n", PSXE_MCD2_PATH);
#endif
    }

    g_boot_stage = 7; /* the sound */

#if PSXE_SOUND == 2
    /* sound to the headphone jack (audio_out.c); without it the game runs silently */
    if (audio_out_init())
        PRINTF("audio: no sound output\r\n");
#endif

    g_psxInitialized = true;
    PRINTF("PSX Emulator fully initialized!\r\n");

    /* Try to load BIOS if path is specified */
    if (g_psxConfig.bios_path)
    {
        PRINTF("Attempting to load BIOS: %s\r\n", g_psxConfig.bios_path);

        /* Check if BIOS file exists first */
        FIL biosFile;
        FRESULT res = f_open(&biosFile, g_psxConfig.bios_path, FA_READ);
        if (res == FR_OK)
        {
            f_close(&biosFile);

            /* Load BIOS using PSX core function */
            PRINTF("Loading BIOS file...\r\n");
            int result = psx_load_bios(g_psx, g_psxConfig.bios_path);
            if (result == 0)
            {
                PRINTF("BIOS loaded successfully\r\n");
            }
            else
            {
                PRINTF("BIOS load failed with error: %d\r\n", result);
            }
        }
        else
        {
            PRINTF("BIOS file not found: %s (Error: %d)\r\n", g_psxConfig.bios_path, res);
            PRINTF("PSX will run without BIOS (may cause issues)\r\n");
        }
    }

//    run_all_tests();

//    /* Load executable if specified */
//    if (g_psxConfig.exe_path)
//    {
//        PRINTF("Loading PSX executable: %s\r\n", g_psxConfig.exe_path);
//        /* Wait for CPU to be ready */
//        while (g_psx->cpu->pc != 0x80030000)
//        {
//            psx_update(g_psx);
//        }
//    }

    /* Main emulation loop */
    g_boot_stage = 8; /* running */
    PRINTF("Starting PSX emulation loop...\r\n");

    psx_prof_init();

    uint32_t pad_tick = 0;

    while (psxe_screen_is_open(g_screen))
    {
        psx_update(g_psx);

        /* A game reads the pad once per frame and the bridge sends at most a
           few hundred frames a second, so looking every few hundred device
           slices is plenty - and costs nothing when nothing has arrived. */
        if ((++pad_tick & 0x1ffu) == 0u)
        {
            psxe_gamepad_poll();

            /* memory card writes to the SD card, once a save is complete */
            if ((pad_tick & 0x3fffu) == 0u)
                psx_pad_tick_mcd(g_psx->pad, (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        }
    }

    /* Cleanup on exit */
    PRINTF("PSX Emulator shutting down...\r\n");

    if (g_psx && g_psx->pad)
    {
        psx_pad_detach_joy(g_psx->pad, 0);
    }

    if (g_psx)
    {
        psx_destroy(g_psx);
        g_psx = NULL;
    }

    if (g_screen)
    {
        psxe_screen_destroy(g_screen);
        g_screen = NULL;
    }

    g_psxInitialized = false;
    PRINTF("PSX Emulator task ended\r\n");

    /* Delete this task */
    vTaskDelete(NULL);
}

/* Where the system stopped, for the probe: configASSERT (FreeRTOSConfig.h) and
   the malloc failed hook come here. Interrupts off and a loop, as before -
   but the place is kept and printed (the debug console writes without them). */
volatile const char *g_assert_file;
volatile int g_assert_line;

void psxe_assert_failed(const char *file, int line)
{
    taskDISABLE_INTERRUPTS();

    g_assert_file = file;
    g_assert_line = line;

    PRINTF("\r\nASSERT failed: %s:%d\r\n", file, line);

    for (;;)
        ;
}

/*!
 * @brief Malloc failed hook.
 */
void vApplicationMallocFailedHook(void)
{
    psxe_assert_failed("FreeRTOS heap: out of memory", (int)xPortGetFreeHeapSize());
}

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    /* If the buffers to be provided to the Idle task are declared inside this
    function then they must be declared static - otherwise they will be allocated on
    the stack and so not exists after this function exits. */
    static StaticTask_t xIdleTaskTCB;
    static StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE];

    /* Pass out a pointer to the StaticTask_t structure in which the Idle task's
    state will be stored. */
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;

    /* Pass out the array that will be used as the Idle task's stack. */
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;

    /* Pass out the size of the array pointed to by *ppxIdleTaskStackBuffer.
    Note that, as the array is necessarily of type StackType_t,
    configMINIMAL_STACK_SIZE is specified in words, not bytes. */
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}
/*-----------------------------------------------------------*/

/* configUSE_STATIC_ALLOCATION and configUSE_TIMERS are both set to 1, so the
application must provide an implementation of vApplicationGetTimerTaskMemory()
to provide the memory that is used by the Timer service task. */
void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                    StackType_t **ppxTimerTaskStackBuffer,
                                    uint32_t *pulTimerTaskStackSize)
{
    /* If the buffers to be provided to the Timer task are declared inside this
    function then they must be declared static - otherwise they will be allocated on
    the stack and so not exists after this function exits. */
    static StaticTask_t xTimerTaskTCB;
    static StackType_t uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];

    /* Pass out a pointer to the StaticTask_t structure in which the Timer
    task's state will be stored. */
    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;

    /* Pass out the array that will be used as the Timer task's stack. */
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;

    /* Pass out the size of the array pointed to by *ppxTimerTaskStackBuffer.
    Note that, as the array is necessarily of type StackType_t,
    configTIMER_TASK_STACK_DEPTH is specified in words, not bytes. */
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}
