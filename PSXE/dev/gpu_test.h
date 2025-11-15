#ifndef GPU_TEST_H
#define GPU_TEST_H

/**
 * @file gpu_test.h
 * @brief Test suite for GPU triangle rendering functions
 * 
 * This test suite validates the gpu_render_triangle function with various scenarios:
 * - Flat colored triangles
 * - Shaded triangles with color interpolation
 * - Clipping to drawing area
 * - Drawing offset handling
 * - Degenerate triangles (zero area)
 * - Triangles outside viewport
 * - Winding order handling
 * - Large triangles
 * - Small triangles
 * - Color format conversion (RGB888 to RGB565)
 */

/**
 * @brief Run all GPU triangle rendering tests
 * 
 * This function executes all test cases and prints results to stdout.
 * Each test validates a specific aspect of triangle rendering including
 * pixel color accuracy, clipping, and edge cases.
 */
void run_all_tests(void);

#endif // GPU_TEST_H
