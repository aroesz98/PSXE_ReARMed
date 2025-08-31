/*
 * Copyright 2023 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PXP_SMART_H
#define PXP_SMART_H

#include <stdbool.h>
#include <stdint.h>

/*******************************************************************************
 * Smart PXP Acceleration API
 * 
 * These functions implement intelligent hardware acceleration that only
 * uses PXP when it provides measurable performance benefits.
 ******************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Smart screen clear using PXP (always beneficial for full screen)
 * @param color RGB565 color value
 * @return true if successful, false if failed
 */
bool PXP_SmartClearScreen(uint16_t color);

/*!
 * @brief Smart rectangle fill - only uses PXP for large rectangles
 * @param x X coordinate
 * @param y Y coordinate  
 * @param width Width in pixels
 * @param height Height in pixels
 * @param color RGB565 color value
 * @return true if PXP was used, false if CPU should be used
 */
bool PXP_SmartFillRect(int x, int y, int width, int height, uint16_t color);

/*!
 * @brief Analyze if triangle should use PXP acceleration
 * @param x1, y1 First vertex
 * @param x2, y2 Second vertex  
 * @param x3, y3 Third vertex
 * @return true if PXP should be used, false if CPU is better
 */
bool PXP_ShouldAccelerateTriangle(int x1, int y1, int x2, int y2, int x3, int y3);

/*!
 * @brief Reset performance statistics
 */
void PXP_ResetStats(void);

/*!
 * @brief Print performance statistics for debugging
 */
void PXP_PrintStats(void);

/*!
 * @brief Demonstrate smart PXP vs naive PXP performance
 * This function shows why selective PXP usage is better
 */
void PXP_PerformanceDemo(void);

#ifdef __cplusplus
}
#endif

#endif /* PXP_SMART_H */
