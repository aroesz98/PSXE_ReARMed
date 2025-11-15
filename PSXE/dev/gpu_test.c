#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "gpu.h"
#include "psx.h"

extern psx_t g_psx_instance;
extern void gpu_render_triangle(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2, poly_data_t data, int edge);
extern void gpu_render_rect(psx_gpu_t *gpu, rect_data_t data);
extern psx_gpu_t *psx_get_gpu(psx_t *psx);

// Global buffer for test messages to avoid stack allocation
static char g_test_msg_buffer[256];

// DWT (Data Watchpoint and Trace) registers for cycle counting
#define DWT_CONTROL             (*((volatile uint32_t*)0xE0001000))
#define DWT_CYCCNT              (*((volatile uint32_t*)0xE0001004))
#define DEM_CR                  (*((volatile uint32_t*)0xE000EDFC))
#define DEM_CR_TRCENA           (1 << 24)

// Initialize DWT for cycle counting
static inline void dwt_init(void) {
    DEM_CR |= DEM_CR_TRCENA;
    DWT_CYCCNT = 0;
    DWT_CONTROL |= 1;
}

// Get current cycle count
static inline uint32_t dwt_get_cycles(void) {
    return DWT_CYCCNT;
}

// Test result structure
typedef struct {
    const char *name;
    int passed;
    const char *message;
    uint32_t render_cycles;  // Cycles for just the render call
} test_result_t;

// Helper function to create and initialize a test GPU
psx_gpu_t* create_test_gpu(void) {
    psx_gpu_t *gpu = psx_get_gpu(&g_psx_instance);
    
    // Set up default drawing area
    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = 1023;
    gpu->draw_y2 = 511;
    
    // Clear drawing offset
    gpu->off_x = 0;
    gpu->off_y = 0;
    
    return gpu;
}

// Helper function to destroy test GPU
void destroy_test_gpu(psx_gpu_t *gpu) {
    if (gpu) {
        if (gpu->vram) {
            free(gpu->vram);
        }
        if (gpu->empty) {
            free(gpu->empty);
        }
        free(gpu);
    }
}

// Helper to convert RGB888 to RGB565
static inline uint16_t rgb888_to_rgb565_test(uint32_t color) {
    uint16_t bgr = ((color & 0x0000f8) >> 3) | ((color & 0x00f800) >> 6) | ((color & 0xf80000) >> 9);
    uint16_t b = (bgr >> 10) & 0x1F;
    uint16_t g = (bgr >> 5) & 0x1F;
    uint16_t r = (bgr >> 0) & 0x1F;
    return (r << 11) | (g << 6) | b;
}

// Helper to check if a pixel is set in VRAM
int is_pixel_set(psx_gpu_t *gpu, int x, int y) {
    if (x < 0 || x >= 1024 || y < 0 || y >= 512) {
        return 0;
    }
    return gpu->vram[x + y * 1024] != 0;
}

// Helper to get pixel color from VRAM
uint16_t get_pixel_color(psx_gpu_t *gpu, int x, int y) {
    if (x < 0 || x >= 1024 || y < 0 || y >= 512) {
        return 0;
    }
    return gpu->vram[x + y * 1024];
}

// Helper to count non-zero pixels in a rectangular region
int count_pixels_in_region(psx_gpu_t *gpu, int x1, int y1, int x2, int y2) {
    int count = 0;
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            if (is_pixel_set(gpu, x, y)) {
                count++;
            }
        }
    }
    return count;
}

// Test 1: Basic flat-colored triangle rendering
test_result_t test_flat_colored_triangle(void) {
    test_result_t result = {"Flat Colored Triangle", 0, ""};
    
    psx_gpu_t *gpu = psx_get_gpu(&g_psx_instance);
    if (!gpu) {
        result.message = "Failed to create GPU";
        return result;
    }
    
    // Clear VRAM first
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    
    // Restore default drawing area (in case other tests modified it)
    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = 1023;
    gpu->draw_y2 = 511;
    gpu->off_x = 0;
    gpu->off_y = 0;
    
    // Create a simple triangle
    vertex_t v0 = {100, 100, 0xFF0000, 0, 0}; // Red
    vertex_t v1 = {200, 100, 0xFF0000, 0, 0};
    vertex_t v2 = {150, 200, 0xFF0000, 0, 0};
    
    poly_data_t data = {0};
    data.attrib = 0; // No texture, no shading, no transparency
    data.v[0] = v0;  // Important: flat_color comes from data.v[0].c
    data.v[1] = v1;
    data.v[2] = v2;
    
    // Render the triangle
    uint32_t render_start = dwt_get_cycles();
    gpu_render_triangle(gpu, v0, v1, v2, data, 0);
    uint32_t render_end = dwt_get_cycles();
    result.render_cycles = render_end - render_start;
    
    // Check that some pixels were rendered
    int pixel_count = count_pixels_in_region(gpu, 100, 100, 200, 200);
    
    if (pixel_count > 0) {
        // Check that center pixel has correct color
        uint16_t center_color = get_pixel_color(gpu, 150, 150);
        uint16_t expected_color = rgb888_to_rgb565_test(0xFF0000);
        
        if (center_color == expected_color) {
            result.passed = 1;
            snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "Triangle rendered with correct color (pixels=%d, color=0x%04X)", pixel_count, center_color);
            result.message = g_test_msg_buffer;
        } else {
            snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "Triangle rendered but color is incorrect (got=0x%04X, expected=0x%04X)", center_color, expected_color);
            result.message = g_test_msg_buffer;
        }
    } else {
        result.message = "No pixels were rendered";
    }

    return result;
}

// Test 2: Shaded triangle with color interpolation
test_result_t test_shaded_triangle(void) {
    test_result_t result = {"Shaded Triangle", 0, ""};
    
    psx_gpu_t *gpu = psx_get_gpu(&g_psx_instance);
    if (!gpu) {
        result.message = "Failed to create GPU";
        return result;
    }
    
    // Clear VRAM first
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    
    // Restore default drawing area (in case other tests modified it)
    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = 1023;
    gpu->draw_y2 = 511;
    gpu->off_x = 0;
    gpu->off_y = 0;
    
    // Create a triangle with different colors at each vertex
    vertex_t v0 = {100, 100, 0xFF0000, 0, 0}; // Red
    vertex_t v1 = {200, 100, 0x00FF00, 0, 0}; // Green
    vertex_t v2 = {150, 200, 0x0000FF, 0, 0}; // Blue
    
    poly_data_t data = {0};
    data.attrib = PA_SHADED;
    data.v[0] = v0;
    data.v[1] = v1;
    data.v[2] = v2;
    
    // Render the triangle
    uint32_t render_start = dwt_get_cycles();
    gpu_render_triangle(gpu, v0, v1, v2, data, 0);
    uint32_t render_end = dwt_get_cycles();
    result.render_cycles = render_end - render_start;
    
    // Check that pixels were rendered
    int pixel_count = count_pixels_in_region(gpu, 100, 100, 200, 200);
    
    if (pixel_count > 0) {
        // Get colors at different positions
        uint16_t top_left = get_pixel_color(gpu, 110, 110);
        uint16_t top_right = get_pixel_color(gpu, 190, 110);
        uint16_t bottom = get_pixel_color(gpu, 150, 190);
        
        // Colors should be different due to interpolation
        if (top_left != top_right && top_left != bottom && top_right != bottom) {
            result.passed = 1;
            snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "Shaded triangle (pixels=%d, TL=0x%04X, TR=0x%04X, B=0x%04X)", 
                     pixel_count, top_left, top_right, bottom);
            result.message = g_test_msg_buffer;
        } else {
            snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "Interpolation incorrect (TL=0x%04X, TR=0x%04X, B=0x%04X)", 
                     top_left, top_right, bottom);
            result.message = g_test_msg_buffer;
        }
    } else {
        result.message = "No pixels were rendered";
    }

    return result;
}

// Test 3: Triangle with drawing area clipping
test_result_t test_triangle_clipping(void) {
    test_result_t result = {"Triangle Clipping", 0, ""};
    
    psx_gpu_t *gpu = psx_get_gpu(&g_psx_instance);
    if (!gpu) {
        result.message = "Failed to create GPU";
        return result;
    }
    
    // Clear VRAM first
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    
    // Set a restricted drawing area
    gpu->draw_x1 = 125;
    gpu->draw_y1 = 125;
    gpu->draw_x2 = 175;
    gpu->draw_y2 = 175;
    
    // Create a triangle that extends beyond the drawing area
    vertex_t v0 = {100, 100, 0xFF0000, 0, 0};
    vertex_t v1 = {200, 100, 0xFF0000, 0, 0};
    vertex_t v2 = {150, 200, 0xFF0000, 0, 0};
    
    poly_data_t data = {0};
    data.attrib = 0;
    data.v[0] = v0;
    
    // Render the triangle
    uint32_t render_start = dwt_get_cycles();
    gpu_render_triangle(gpu, v0, v1, v2, data, 0);
    uint32_t render_end = dwt_get_cycles();
    result.render_cycles = render_end - render_start;
    
    // Check that no pixels were rendered outside the clipping area
    int outside_count = 0;
    for (int y = 100; y <= 200; y++) {
        for (int x = 100; x <= 200; x++) {
            if ((x < gpu->draw_x1 || x > gpu->draw_x2 || 
                 y < gpu->draw_y1 || y > gpu->draw_y2) && 
                is_pixel_set(gpu, x, y)) {
                outside_count++;
            }
        }
    }
    
    // Check that some pixels were rendered inside the clipping area
    int inside_count = count_pixels_in_region(gpu, 125, 125, 175, 175);
    
    if (outside_count == 0 && inside_count > 0) {
        result.passed = 1;
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "Triangle clipped correctly (inside=%d, outside=%d)", inside_count, outside_count);
        result.message = g_test_msg_buffer;
    } else if (outside_count > 0) {
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "Pixels outside clipping area (inside=%d, outside=%d)", inside_count, outside_count);
        result.message = g_test_msg_buffer;
    } else {
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "No pixels rendered (inside=%d, outside=%d)", inside_count, outside_count);
        result.message = g_test_msg_buffer;
    }
    
    // Restore default drawing area
    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = 1023;
    gpu->draw_y2 = 511;
    
    return result;
}

// Test 4: Triangle with drawing offset
test_result_t test_triangle_offset(void) {
    test_result_t result = {"Triangle Drawing Offset", 0, ""};
    
    psx_gpu_t *gpu = psx_get_gpu(&g_psx_instance);
    if (!gpu) {
        result.message = "Failed to create GPU";
        return result;
    }
    
    // Clear VRAM first
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    
    // Restore default drawing area
    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = 1023;
    gpu->draw_y2 = 511;
    
    // Set drawing offset
    gpu->off_x = 50;
    gpu->off_y = 50;
    
    // Create a triangle
    vertex_t v0 = {100, 100, 0xFF0000, 0, 0};
    vertex_t v1 = {200, 100, 0xFF0000, 0, 0};
    vertex_t v2 = {150, 200, 0xFF0000, 0, 0};
    
    poly_data_t data = {0};
    data.attrib = 0;
    data.v[0] = v0;
    data.v[1] = v1;
    data.v[2] = v2;
    
    // Render the triangle
    uint32_t render_start = dwt_get_cycles();
    gpu_render_triangle(gpu, v0, v1, v2, data, 0);
    uint32_t render_end = dwt_get_cycles();
    result.render_cycles = render_end - render_start;
    
    // Check that pixels were rendered at offset positions
    // With offset (50,50), the triangle at (100,100)-(200,100)-(150,200) 
    // renders at (150,150)-(250,150)-(200,250)
    // Check a point that should be inside the offset triangle
    uint16_t offset_pixel = get_pixel_color(gpu, 180, 170);
    
    // Check a point where the original (non-offset) triangle would be
    // but the offset triangle is not (e.g., left side of original triangle)
    uint16_t original_pixel = get_pixel_color(gpu, 120, 120);
    
    if (offset_pixel != 0 && original_pixel == 0) {
        result.passed = 1;
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "Offset correct (offset@180,170=0x%04X, original@120,120=0x%04X)", 
                 offset_pixel, original_pixel);
        result.message = g_test_msg_buffer;
    } else if (offset_pixel == 0) {
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "No pixels at offset (offset@180,170=0x%04X, original@120,120=0x%04X)", 
                 offset_pixel, original_pixel);
        result.message = g_test_msg_buffer;
    } else {
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "Both positions have pixels (offset@180,170=0x%04X, original@120,120=0x%04X)", 
                 offset_pixel, original_pixel);
        result.message = g_test_msg_buffer;
    }
    
    // Restore offset to 0
    gpu->off_x = 0;
    gpu->off_y = 0;
    
    return result;
}

// Test 5: Degenerate triangle (zero area)
test_result_t test_degenerate_triangle(void) {
    test_result_t result = {"Degenerate Triangle", 0, ""};
    
    psx_gpu_t *gpu = psx_get_gpu(&g_psx_instance);
    if (!gpu) {
        result.message = "Failed to create GPU";
        return result;
    }
    
    // Clear VRAM first
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    
    // Create a degenerate triangle (all points on a line)
    vertex_t v0 = {100, 100, 0xFF0000, 0, 0};
    vertex_t v1 = {200, 100, 0xFF0000, 0, 0};
    vertex_t v2 = {150, 100, 0xFF0000, 0, 0};
    
    poly_data_t data = {0};
    data.attrib = 0;
    data.v[0] = v0;
    
    // Render the triangle
    uint32_t render_start = dwt_get_cycles();
    gpu_render_triangle(gpu, v0, v1, v2, data, 0);
    uint32_t render_end = dwt_get_cycles();
    result.render_cycles = render_end - render_start;
    
    // Check that no pixels were rendered (or very few)
    int pixel_count = count_pixels_in_region(gpu, 90, 90, 210, 110);
    
    if (pixel_count == 0) {
        result.passed = 1;
        result.message = "Degenerate triangle correctly not rendered";
    } else {
        result.message = "Degenerate triangle rendered some pixels";
    }

    return result;
}

// Test 6: Triangle completely outside drawing area
test_result_t test_triangle_outside_viewport(void) {
    test_result_t result = {"Triangle Outside Viewport", 0, ""};
    
    psx_gpu_t *gpu = psx_get_gpu(&g_psx_instance);
    if (!gpu) {
        result.message = "Failed to create GPU";
        return result;
    }
    
    // Clear VRAM first
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    
    // Create a triangle completely outside the viewport
    vertex_t v0 = {-100, -100, 0xFF0000, 0, 0};
    vertex_t v1 = {-50, -100, 0xFF0000, 0, 0};
    vertex_t v2 = {-75, -50, 0xFF0000, 0, 0};
    
    poly_data_t data = {0};
    data.attrib = 0;
    data.v[0] = v0;
    
    // Render the triangle
    uint32_t render_start = dwt_get_cycles();
    gpu_render_triangle(gpu, v0, v1, v2, data, 0);
    uint32_t render_end = dwt_get_cycles();
    result.render_cycles = render_end - render_start;
    
    // Check that no pixels were rendered
    int pixel_count = count_pixels_in_region(gpu, 0, 0, 1023, 511);
    
    if (pixel_count == 0) {
        result.passed = 1;
        result.message = "Triangle outside viewport correctly not rendered";
    } else {
        result.message = "Triangle outside viewport rendered pixels";
    }
    
    return result;
}

// Test 7: Winding order test (clockwise vs counter-clockwise)
test_result_t test_triangle_winding_order(void) {
    test_result_t result = {"Triangle Winding Order", 0, ""};
    
    psx_gpu_t *gpu = psx_get_gpu(&g_psx_instance);
    if (!gpu) {
        result.message = "Failed to create GPU";
        return result;
    }
    
    // Clear VRAM first
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    
    // Test counter-clockwise winding
    vertex_t v0_ccw = {100, 100, 0xFF0000, 0, 0};
    vertex_t v1_ccw = {200, 100, 0xFF0000, 0, 0};
    vertex_t v2_ccw = {150, 200, 0xFF0000, 0, 0};
    
    poly_data_t data = {0};
    data.attrib = 0;
    data.v[0] = v0_ccw;
    
    uint32_t render_start = dwt_get_cycles();
    gpu_render_triangle(gpu, v0_ccw, v1_ccw, v2_ccw, data, 0);
    uint32_t render_end = dwt_get_cycles();
    result.render_cycles = render_end - render_start;
    
    int ccw_pixels = count_pixels_in_region(gpu, 100, 100, 200, 200);
    
    // Clear VRAM
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    
    // Test clockwise winding (swap v1 and v2)
    gpu_render_triangle(gpu, v0_ccw, v2_ccw, v1_ccw, data, 0);
    
    int cw_pixels = count_pixels_in_region(gpu, 100, 100, 200, 200);
    
    if (ccw_pixels > 0 && cw_pixels > 0) {
        result.passed = 1;
        result.message = "Triangle handles both winding orders";
    } else if (ccw_pixels == 0 && cw_pixels == 0) {
        result.message = "No pixels rendered for either winding order";
    } else {
        result.message = "Only one winding order renders pixels";
    }
    
    return result;
}

// Test 8: Large triangle test
test_result_t test_large_triangle(void) {
    test_result_t result = {"Large Triangle", 0, ""};
    
    psx_gpu_t *gpu = psx_get_gpu(&g_psx_instance);
    if (!gpu) {
        result.message = "Failed to create GPU";
        return result;
    }
    
    // Clear VRAM first
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    
    // Create a large triangle covering most of the screen
    vertex_t v0 = {0, 0, 0xFF0000, 0, 0};
    vertex_t v1 = {1023, 0, 0xFF0000, 0, 0};
    vertex_t v2 = {512, 511, 0xFF0000, 0, 0};
    
    poly_data_t data = {0};
    data.attrib = 0;
    data.v[0] = v0;
    
    // Render the triangle
    uint32_t render_start = dwt_get_cycles();
    gpu_render_triangle(gpu, v0, v1, v2, data, 0);
    uint32_t render_end = dwt_get_cycles();
    result.render_cycles = render_end - render_start;
    
    // Check that a significant number of pixels were rendered
    int pixel_count = count_pixels_in_region(gpu, 0, 0, 1023, 511);
    
    // Expected rough area: 1024 * 512 / 2 = 262144
    if (pixel_count > 100000) {
        result.passed = 1;
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "Large triangle rendered (pixels=%d, expected~262144)", pixel_count);
        result.message = g_test_msg_buffer;
    } else if (pixel_count > 0) {
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "Large triangle pixel count low (pixels=%d, expected>100000)", pixel_count);
        result.message = g_test_msg_buffer;
    } else {
        result.message = "Large triangle not rendered (pixels=0)";
    }
    
    return result;
}

// Test 9: Small triangle test (single pixel)
test_result_t test_small_triangle(void) {
    test_result_t result = {"Small Triangle", 0, ""};
    
    psx_gpu_t *gpu = psx_get_gpu(&g_psx_instance);
    if (!gpu) {
        result.message = "Failed to create GPU";
        return result;
    }
    
    // Clear VRAM first
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    
    // Create a very small triangle
    vertex_t v0 = {100, 100, 0xFF0000, 0, 0};
    vertex_t v1 = {102, 100, 0xFF0000, 0, 0};
    vertex_t v2 = {101, 102, 0xFF0000, 0, 0};
    
    poly_data_t data = {0};
    data.attrib = 0;
    data.v[0] = v0;
    
    // Render the triangle
    uint32_t render_start = dwt_get_cycles();
    gpu_render_triangle(gpu, v0, v1, v2, data, 0);
    uint32_t render_end = dwt_get_cycles();
    result.render_cycles = render_end - render_start;
    
    // Check that at least one pixel was rendered
    int pixel_count = count_pixels_in_region(gpu, 99, 99, 103, 103);
    
    if (pixel_count > 0) {
        result.passed = 1;
        result.message = "Small triangle rendered successfully";
    } else {
        result.message = "Small triangle not rendered";
    }
    
    return result;
}

// Test 10: Color format test (RGB888 to RGB565 conversion)
test_result_t test_color_format_conversion(void) {
    test_result_t result = {"Color Format Conversion", 0, ""};
    
    psx_gpu_t *gpu = psx_get_gpu(&g_psx_instance);
    if (!gpu) {
        result.message = "Failed to create GPU";
        return result;
    }
    
    // Clear VRAM first
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    
    // Test pure red
    vertex_t v0 = {100, 100, 0xFF0000, 0, 0};
    vertex_t v1 = {200, 100, 0xFF0000, 0, 0};
    vertex_t v2 = {150, 200, 0xFF0000, 0, 0};
    
    poly_data_t data = {0};
    data.attrib = 0;
    data.v[0] = v0;
    data.v[1] = v1;
    data.v[2] = v2;
    
    uint32_t render_start = dwt_get_cycles();
    gpu_render_triangle(gpu, v0, v1, v2, data, 0);
    uint32_t render_end = dwt_get_cycles();
    result.render_cycles = render_end - render_start;
    
    uint16_t red_pixel = get_pixel_color(gpu, 150, 150);
    uint16_t expected_red = rgb888_to_rgb565_test(0xFF0000);
    
    // Clear and test green
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    v0.c = v1.c = v2.c = 0x00FF00;
    data.v[0] = v0;
    data.v[1] = v1;
    data.v[2] = v2;
    
    gpu_render_triangle(gpu, v0, v1, v2, data, 0);
    
    uint16_t green_pixel = get_pixel_color(gpu, 150, 150);
    uint16_t expected_green = rgb888_to_rgb565_test(0x00FF00);
    
    // Clear and test blue
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    v0.c = v1.c = v2.c = 0x0000FF;
    data.v[0] = v0;
    data.v[1] = v1;
    data.v[2] = v2;
    
    gpu_render_triangle(gpu, v0, v1, v2, data, 0);
    
    uint16_t blue_pixel = get_pixel_color(gpu, 150, 150);
    uint16_t expected_blue = rgb888_to_rgb565_test(0x0000FF);
    
    if (red_pixel == expected_red && green_pixel == expected_green && blue_pixel == expected_blue) {
        result.passed = 1;
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "Color conversion OK (R:0x%04X, G:0x%04X, B:0x%04X)", 
                 red_pixel, green_pixel, blue_pixel);
        result.message = g_test_msg_buffer;
    } else {
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer), "Color errors (R:0x%04X/0x%04X, G:0x%04X/0x%04X, B:0x%04X/0x%04X)", 
                 red_pixel, expected_red, green_pixel, expected_green, blue_pixel, expected_blue);
        result.message = g_test_msg_buffer;
    }
    
    return result;
}

// Test 11: Solid rectangle fill
test_result_t test_rect_solid_fill(void) {
    test_result_t result = {"Rectangle Solid Fill", 0, ""};

    psx_gpu_t *gpu = psx_get_gpu(&g_psx_instance);
    if (!gpu) {
        result.message = "Failed to create GPU";
        return result;
    }

    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = 1023;
    gpu->draw_y2 = 511;
    gpu->off_x = 0;
    gpu->off_y = 0;

    rect_data_t rect = {0};
    rect.attrib = 0;
    rect.v0.x = 160;
    rect.v0.y = 120;
    rect.v0.c = 0x00FF00;
    rect.width = 40;
    rect.height = 32;

    uint32_t render_start = dwt_get_cycles();
    gpu_render_rect(gpu, rect);
    uint32_t render_end = dwt_get_cycles();
    result.render_cycles = render_end - render_start;

    const int x0 = rect.v0.x;
    const int y0 = rect.v0.y;
    const int x1 = x0 + rect.width - 1;
    const int y1 = y0 + rect.height - 1;
    const int expected_pixels = rect.width * rect.height;
    const int pixel_count = count_pixels_in_region(gpu, x0, y0, x1, y1);
    const uint16_t center_color = get_pixel_color(gpu, x0 + rect.width / 2, y0 + rect.height / 2);
    const uint16_t expected_color = rgb888_to_rgb565_test(rect.v0.c);

    if (pixel_count == expected_pixels && center_color == expected_color) {
        result.passed = 1;
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer),
                 "Rect fill OK (pixels=%d, expected=%d, color=0x%04X)",
                 pixel_count, expected_pixels, center_color);
        result.message = g_test_msg_buffer;
    } else {
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer),
                 "Rect fill mismatch (pixels=%d/%d, color=0x%04X/0x%04X)",
                 pixel_count, expected_pixels, center_color, expected_color);
        result.message = g_test_msg_buffer;
    }

    return result;
}

// Test 12: Rectangle clipping against drawing area
test_result_t test_rect_clipping(void) {
    test_result_t result = {"Rectangle Clipping", 0, ""};

    psx_gpu_t *gpu = psx_get_gpu(&g_psx_instance);
    if (!gpu) {
        result.message = "Failed to create GPU";
        return result;
    }

    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);

    gpu->draw_x1 = 200;
    gpu->draw_y1 = 150;
    gpu->draw_x2 = 260;
    gpu->draw_y2 = 210;

    rect_data_t rect = {0};
    rect.attrib = 0;
    rect.v0.x = 180;
    rect.v0.y = 130;
    rect.v0.c = 0x0000FF;
    rect.width = 120;
    rect.height = 120;

    uint32_t render_start = dwt_get_cycles();
    gpu_render_rect(gpu, rect);
    uint32_t render_end = dwt_get_cycles();
    result.render_cycles = render_end - render_start;

    int outside_pixels = 0;
    for (int y = rect.v0.y; y < rect.v0.y + rect.height; ++y) {
        for (int x = rect.v0.x; x < rect.v0.x + rect.width; ++x) {
            if (is_pixel_set(gpu, x, y) &&
                (x < (int)gpu->draw_x1 || x > (int)gpu->draw_x2 ||
                 y < (int)gpu->draw_y1 || y > (int)gpu->draw_y2)) {
                outside_pixels++;
            }
        }
    }

    const int inside_pixels = count_pixels_in_region(gpu,
                                                     gpu->draw_x1,
                                                     gpu->draw_y1,
                                                     gpu->draw_x2,
                                                     gpu->draw_y2);
    const uint16_t inside_color = get_pixel_color(gpu, 220, 180);
    const uint16_t expected_color = rgb888_to_rgb565_test(rect.v0.c);

    if (outside_pixels == 0 && inside_pixels > 0 && inside_color == expected_color) {
        result.passed = 1;
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer),
                 "Rect clipped (inside=%d, outside=%d)", inside_pixels, outside_pixels);
        result.message = g_test_msg_buffer;
    } else {
        snprintf(g_test_msg_buffer, sizeof(g_test_msg_buffer),
                 "Rect clip mismatch (inside=%d, outside=%d, color=0x%04X/0x%04X)",
                 inside_pixels, outside_pixels, inside_color, expected_color);
        result.message = g_test_msg_buffer;
    }

    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = 1023;
    gpu->draw_y2 = 511;

    return result;
}

// Main test runner
void run_all_tests(void) {
    printf("\n========================================\n");
    printf("GPU Triangle Rendering Tests\n");
    printf("========================================\n\n");
    
    // Initialize DWT for cycle counting
    // dwt_init();
    
    test_result_t results[12];
    int total_tests = 0;
    int passed_tests = 0;
    
    results[total_tests++] = test_flat_colored_triangle();
    results[total_tests++] = test_shaded_triangle();
    results[total_tests++] = test_triangle_clipping();
    results[total_tests++] = test_triangle_offset();
    results[total_tests++] = test_degenerate_triangle();
    results[total_tests++] = test_triangle_outside_viewport();
    results[total_tests++] = test_triangle_winding_order();
    results[total_tests++] = test_large_triangle();
    results[total_tests++] = test_small_triangle();
    results[total_tests++] = test_color_format_conversion();
    results[total_tests++] = test_rect_solid_fill();
    results[total_tests++] = test_rect_clipping();
    
    // Print results
    for (int i = 0; i < total_tests; i++) {
        const char *status = results[i].passed ? "[PASS]" : "[FAIL]";
        printf("%s Test %d: %s\n", status, i + 1, results[i].name);
        printf("       %s\n", results[i].message);
        printf("       Render: %lu cycles (%.2f ms @ 600MHz)\n\n", 
               (unsigned long)results[i].render_cycles, 
               results[i].render_cycles / 600000.0f);
        
        if (results[i].passed) {
            passed_tests++;
        }
    }
    
    printf("========================================\n");
    printf("Results: %d/%d tests passed (%.1f%%)\n", 
           passed_tests, total_tests, 
           (float)passed_tests / total_tests * 100.0f);
    printf("========================================\n\n");
}
