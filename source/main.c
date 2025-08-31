/*
 * Copyright 2020 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

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
#include "board.h"
#include "fsl_gpio.h"

/* DWT cycle counter for performance monitoring */
#include "core_cm7.h"

/* PSX Emulator includes */
#include "psx.h"
#include "input/sda.h"
#include "input/guncon.h"
#include "dev/cdrom/cdrom.h"
#include "dev/timer.h"
#include "cpu.h"
#include "frontend/screen.h"
#include "log.h"

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

/* PSX Emulator global variables */
static psx_t *g_psx = NULL;
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

    PRINTF("PSXE MCU Emulator Starting...\r\n");

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

    /* Set PSX emulator log level to reduce verbosity */
    log_set_level(g_psxConfig.log_level);
    PRINTF("PSX log level set to: %d (ERROR and FATAL only)\r\n", g_psxConfig.log_level);

    /* Initialize PSX emulator */
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
        PRINTF("Input system initialized\r\n");
    }

    /* Memory card support (commented out for now) */
    /* psx_pad_attach_mcd(g_psx->pad, 0, "slot1.mcd"); */
    /* psx_pad_attach_mcd(g_psx->pad, 1, "slot2.mcd"); */

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

    /* Load executable if specified */
    if (g_psxConfig.exe_path)
    {
        PRINTF("Loading PSX executable: %s\r\n", g_psxConfig.exe_path);
        /* Wait for CPU to be ready */
        while (g_psx->cpu->pc != 0x80030000)
        {
            psx_update(g_psx);
        }
    }

    /* Main emulation loop */
    PRINTF("Starting PSX emulation loop...\r\n");

    while (psxe_screen_is_open(g_screen))
    {
        psx_update(g_psx);
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

/*!
 * @brief Malloc failed hook.
 */
void vApplicationMallocFailedHook(void)
{
    for (;;)
        ;
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
