/*
 *  src/ui_button_mask.c — button regions from the frame's button mask, and
 *  hit-testing against them. Split out of src/ui.c; no SDL.
 */

#include "ui_button_mask.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define UI_BUTTON_MASK_THRESHOLD 128u
#define UI_BUTTON_MASK_MIN_AREA 500u
#define UI_BUTTON_MASK_MAX_COMPONENTS 16u

typedef struct {
    uint32_t area;
    uint32_t min_x;
    uint32_t min_y;
    uint32_t max_x;
    uint32_t max_y;
    double cx;
    double cy;
} ui_button_component_t;

static int ui_button_component_area_desc(const void *ap, const void *bp)
{
    const ui_button_component_t *a = (const ui_button_component_t *)ap;
    const ui_button_component_t *b = (const ui_button_component_t *)bp;

    if (a->area < b->area)
        return 1;
    if (a->area > b->area)
        return -1;
    return 0;
}

static void ui_insert_button_component(ui_button_component_t *components,
    size_t *count_io, const ui_button_component_t *component)
{
    size_t count = *count_io;

    if (component->area < UI_BUTTON_MASK_MIN_AREA)
        return;

    if (count < UI_BUTTON_MASK_MAX_COMPONENTS) {
        components[count] = *component;
        *count_io = count + 1u;
        return;
    }

    size_t smallest = 0;
    for (size_t i = 1; i < count; i++) {
        if (components[i].area < components[smallest].area)
            smallest = i;
    }
    if (component->area > components[smallest].area)
        components[smallest] = *component;
}

static bool ui_collect_button_components(const uint8_t *active,
    uint32_t width, uint32_t height, ui_button_component_t *components,
    size_t *count_out)
{
    size_t pixel_count = (size_t)width * (size_t)height;
    uint8_t *seen = NULL;
    uint32_t *queue = NULL;
    size_t count = 0;

    *count_out = 0;
    if (!active || width == 0 || height == 0)
        return false;

    seen = calloc(pixel_count, 1);
    queue = malloc(pixel_count * sizeof(*queue));
    if (!seen || !queue) {
        free(seen);
        free(queue);
        return false;
    }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t start = y * width + x;
            uint32_t head = 0, tail = 0, area = 0;
            uint32_t min_x = x, min_y = y, max_x = x, max_y = y;
            uint64_t sum_x = 0, sum_y = 0;

            if (!active[start] || seen[start])
                continue;

            seen[start] = 1;
            queue[tail++] = start;

            while (head < tail) {
                uint32_t p = queue[head++];
                uint32_t px = p % width;
                uint32_t py = p / width;
                uint32_t y0 = py > 0 ? py - 1u : py;
                uint32_t y1 = py + 1u < height ? py + 1u : py;
                uint32_t x0 = px > 0 ? px - 1u : px;
                uint32_t x1 = px + 1u < width ? px + 1u : px;

                area++;
                sum_x += px;
                sum_y += py;
                if (px < min_x) min_x = px;
                if (px > max_x) max_x = px;
                if (py < min_y) min_y = py;
                if (py > max_y) max_y = py;

                for (uint32_t ny = y0; ny <= y1; ny++) {
                    for (uint32_t nx = x0; nx <= x1; nx++) {
                        uint32_t np;

                        if (nx == px && ny == py)
                            continue;
                        np = ny * width + nx;
                        if (!active[np] || seen[np])
                            continue;
                        seen[np] = 1;
                        queue[tail++] = np;
                    }
                }
            }

            if (area >= UI_BUTTON_MASK_MIN_AREA) {
                ui_button_component_t component = {
                    .area = area,
                    .min_x = min_x,
                    .min_y = min_y,
                    .max_x = max_x,
                    .max_y = max_y,
                    .cx = (double)sum_x / (double)area,
                    .cy = (double)sum_y / (double)area
                };
                ui_insert_button_component(components, &count,
                    &component);
            }
        }
    }

    free(seen);
    free(queue);

    if (count == 0)
        return false;

    qsort(components, count, sizeof(*components),
        ui_button_component_area_desc);
    *count_out = count;
    return true;
}

static void ui_region_from_component(ui_button_region_t *region,
    const ui_button_component_t *component)
{
    region->valid = true;
    region->area = component->area;
    region->min_x = component->min_x;
    region->min_y = component->min_y;
    region->max_x = component->max_x;
    region->max_y = component->max_y;
    region->cx = component->cx;
    region->cy = component->cy;
}

static void ui_split_side_component(const uint8_t *active, uint32_t width,
    const ui_button_component_t *component, bool left_side,
    ui_button_region_t *upper, ui_button_region_t *lower)
{
    double box_w = (double)(component->max_x - component->min_x + 1u);
    double box_h = (double)(component->max_y - component->min_y + 1u);
    double upper_x = (double)component->min_x +
        (left_side ? 0.25 : 0.75) * box_w;
    double upper_y = (double)component->min_y + 0.30 * box_h;
    double lower_x = (double)component->min_x +
        (left_side ? 0.75 : 0.25) * box_w;
    double lower_y = (double)component->min_y + 0.82 * box_h;

    for (int iter = 0; iter < 8; iter++) {
        double upper_sum_x = 0.0, upper_sum_y = 0.0;
        double lower_sum_x = 0.0, lower_sum_y = 0.0;
        uint32_t upper_count = 0, lower_count = 0;

        for (uint32_t y = component->min_y; y <= component->max_y; y++) {
            for (uint32_t x = component->min_x; x <= component->max_x; x++) {
                double du, dl;

                if (!active[(size_t)y * width + x])
                    continue;

                du = ((double)x - upper_x) * ((double)x - upper_x) +
                    ((double)y - upper_y) * ((double)y - upper_y);
                dl = ((double)x - lower_x) * ((double)x - lower_x) +
                    ((double)y - lower_y) * ((double)y - lower_y);
                if (du <= dl) {
                    upper_sum_x += x;
                    upper_sum_y += y;
                    upper_count++;
                } else {
                    lower_sum_x += x;
                    lower_sum_y += y;
                    lower_count++;
                }
            }
        }

        if (upper_count != 0) {
            upper_x = upper_sum_x / (double)upper_count;
            upper_y = upper_sum_y / (double)upper_count;
        }
        if (lower_count != 0) {
            lower_x = lower_sum_x / (double)lower_count;
            lower_y = lower_sum_y / (double)lower_count;
        }
    }

    ui_region_from_component(upper, component);
    ui_region_from_component(lower, component);
    upper->cx = upper_x;
    upper->cy = upper_y;
    lower->cx = lower_x;
    lower->cy = lower_y;
}

static bool ui_assign_button_regions(const ui_button_component_t *components,
    size_t count, const uint8_t *active, uint32_t width,
    ui_button_region_t *regions_out)
{
    int left_upper = -1, left_lower = -1;
    int right_upper = -1, right_lower = -1;
    size_t left_count = 0, right_count = 0;
    const ui_button_component_t *dpad;

    memset(regions_out, 0,
        sizeof(*regions_out) * UI_BUTTON_REGION_COUNT);
    if (!components || count < 3 || !active || width == 0)
        return false;

    dpad = &components[0];
    if (count < 5) {
        int left_side = -1, right_side = -1;

        for (size_t i = 1; i < count; i++) {
            if (components[i].cx < dpad->cx) {
                if (left_side < 0 ||
                    components[i].area > components[left_side].area)
                    left_side = (int)i;
            } else {
                if (right_side < 0 ||
                    components[i].area > components[right_side].area)
                    right_side = (int)i;
            }
        }

        if (left_side < 0 || right_side < 0)
            return false;

        ui_region_from_component(&regions_out[UI_BUTTON_REGION_DPAD],
            dpad);
        ui_split_side_component(active, width, &components[left_side],
            true, &regions_out[UI_BUTTON_REGION_ROCKET],
            &regions_out[UI_BUTTON_REGION_OK]);
        ui_split_side_component(active, width, &components[right_side],
            false, &regions_out[UI_BUTTON_REGION_POWER],
            &regions_out[UI_BUTTON_REGION_ESC]);
        return true;
    }

    for (size_t i = 1; i < count; i++) {
        const ui_button_component_t *component = &components[i];

        if (component->cx < dpad->cx) {
            left_count++;
            if (left_upper < 0 ||
                component->cy < components[left_upper].cy)
                left_upper = (int)i;
            if (left_lower < 0 ||
                component->cy > components[left_lower].cy)
                left_lower = (int)i;
        } else {
            right_count++;
            if (right_upper < 0 ||
                component->cy < components[right_upper].cy)
                right_upper = (int)i;
            if (right_lower < 0 ||
                component->cy > components[right_lower].cy)
                right_lower = (int)i;
        }
    }

    if (left_count < 2 || right_count < 2 ||
        left_upper < 0 || left_lower < 0 ||
        right_upper < 0 || right_lower < 0 ||
        left_upper == left_lower || right_upper == right_lower)
        return false;

    ui_region_from_component(&regions_out[UI_BUTTON_REGION_DPAD], dpad);
    ui_region_from_component(&regions_out[UI_BUTTON_REGION_ROCKET],
        &components[left_upper]);
    ui_region_from_component(&regions_out[UI_BUTTON_REGION_OK],
        &components[left_lower]);
    ui_region_from_component(&regions_out[UI_BUTTON_REGION_POWER],
        &components[right_upper]);
    ui_region_from_component(&regions_out[UI_BUTTON_REGION_ESC],
        &components[right_lower]);
    return true;
}

bool ui_button_mask_build(ui_button_mask_t *mask, const uint8_t *rgba,
    uint32_t width, uint32_t height)
{
    size_t pixel_count = (size_t)width * (size_t)height;
    uint8_t *active = NULL;
    ui_button_component_t components[UI_BUTTON_MASK_MAX_COMPONENTS];
    ui_button_region_t regions[UI_BUTTON_REGION_COUNT];
    size_t component_count = 0;

    if (!rgba || width == 0 || height == 0)
        return false;

    active = malloc(pixel_count);
    if (!active)
        return false;

    for (size_t i = 0; i < pixel_count; i++) {
        const uint8_t *p = rgba + i * 4u;
        uint32_t luma = (uint32_t)p[0] * 77u +
            (uint32_t)p[1] * 150u + (uint32_t)p[2] * 29u;

        active[i] = (p[3] >= 16u &&
            (luma >> 8) >= UI_BUTTON_MASK_THRESHOLD) ? 1u : 0u;
    }

    if (!ui_collect_button_components(active, width, height, components,
        &component_count) ||
        !ui_assign_button_regions(components, component_count, active,
            width, regions)) {
        free(active);
        return false;
    }

    ui_button_mask_free(mask);
    mask->active = active;
    mask->width = width;
    mask->height = height;
    memcpy(mask->regions, regions, sizeof(mask->regions));
    return true;
}

void ui_button_mask_free(ui_button_mask_t *mask)
{
    free(mask->active);
    memset(mask, 0, sizeof(*mask));
}

static bool ui_button_region_contains(const ui_button_region_t *region,
    uint32_t x, uint32_t y)
{
    return region->valid &&
        x >= region->min_x && x <= region->max_x &&
        y >= region->min_y && y <= region->max_y;
}

static double ui_button_region_distance_sq(const ui_button_region_t *region,
    uint32_t x, uint32_t y)
{
    double dx = (double)x + 0.5 - region->cx;
    double dy = (double)y + 0.5 - region->cy;

    return dx * dx + dy * dy;
}

static void ui_set_side_button_from_regions(
    const ui_button_region_t *upper, const ui_button_region_t *lower,
    uint32_t x, uint32_t y, uint8_t upper_btn_set1,
    uint8_t upper_btn_set2, uint8_t lower_btn_set1,
    uint8_t lower_btn_set2, uint8_t *btn_set1_out,
    uint8_t *btn_set2_out)
{
    bool use_upper = ui_button_region_distance_sq(upper, x, y) <=
        ui_button_region_distance_sq(lower, x, y);

    if (use_upper) {
        *btn_set1_out = upper_btn_set1;
        *btn_set2_out = upper_btn_set2;
    } else {
        *btn_set1_out = lower_btn_set1;
        *btn_set2_out = lower_btn_set2;
    }
}

bool ui_button_mask_hit(const ui_button_mask_t *mask, double bx, double by,
    uint8_t *btn_set1_out, uint8_t *btn_set2_out)
{
    int ix = (int)floor(bx);
    int iy = (int)floor(by);
    ui_button_region_kind_t kind = UI_BUTTON_REGION_NONE;
    const ui_button_region_t *region = NULL;

    if (!mask->active || ix < 0 || iy < 0 ||
        (uint32_t)ix >= mask->width ||
        (uint32_t)iy >= mask->height)
        return false;

    if (!mask->active[(size_t)iy * mask->width + (uint32_t)ix])
        return false;

    if (ui_button_region_contains(&mask->regions[UI_BUTTON_REGION_DPAD],
        (uint32_t)ix, (uint32_t)iy)) {
        kind = UI_BUTTON_REGION_DPAD;
        region = &mask->regions[UI_BUTTON_REGION_DPAD];
    } else if (ui_button_region_contains(
        &mask->regions[UI_BUTTON_REGION_ROCKET], (uint32_t)ix,
        (uint32_t)iy) &&
        ui_button_region_contains(&mask->regions[UI_BUTTON_REGION_OK],
        (uint32_t)ix, (uint32_t)iy)) {
        ui_set_side_button_from_regions(
            &mask->regions[UI_BUTTON_REGION_ROCKET],
            &mask->regions[UI_BUTTON_REGION_OK],
            (uint32_t)ix, (uint32_t)iy,
            0, 0x10u, 0x04u, 0, btn_set1_out, btn_set2_out);
        return true;
    } else if (ui_button_region_contains(
        &mask->regions[UI_BUTTON_REGION_POWER], (uint32_t)ix,
        (uint32_t)iy) &&
        ui_button_region_contains(&mask->regions[UI_BUTTON_REGION_ESC],
        (uint32_t)ix, (uint32_t)iy)) {
        ui_set_side_button_from_regions(
            &mask->regions[UI_BUTTON_REGION_POWER],
            &mask->regions[UI_BUTTON_REGION_ESC],
            (uint32_t)ix, (uint32_t)iy,
            0, 0x80u, 0x08u, 0, btn_set1_out, btn_set2_out);
        return true;
    } else {
        for (ui_button_region_kind_t i = UI_BUTTON_REGION_ROCKET;
            i < UI_BUTTON_REGION_COUNT; i++) {
            if (ui_button_region_contains(&mask->regions[i],
                (uint32_t)ix, (uint32_t)iy)) {
                kind = i;
                region = &mask->regions[i];
                break;
            }
        }
    }

    if (!region)
        return false;

    switch (kind) {
    case UI_BUTTON_REGION_DPAD: {
        double rx = ((double)(region->max_x - region->min_x + 1u)) / 2.0;
        double ry = ((double)(region->max_y - region->min_y + 1u)) / 2.0;
        double dx, dy;

        if (rx <= 0.0 || ry <= 0.0)
            return false;

        dx = ((double)ix + 0.5 - region->cx) / rx;
        dy = ((double)iy + 0.5 - region->cy) / ry;
        if (dx * dx + dy * dy < 0.18) {
            *btn_set1_out = 0x04u;  /* ok */
        } else if (fabs(dy) >= fabs(dx)) {
            *btn_set1_out = dy < 0.0 ? 0x10u : 0x20u;  /* up/down */
        } else {
            *btn_set1_out = dx > 0.0 ? 0x40u : 0x80u;  /* right/left */
        }
        return true;
    }
    case UI_BUTTON_REGION_ROCKET:
        *btn_set2_out = 0x10u;
        return true;
    case UI_BUTTON_REGION_POWER:
        *btn_set2_out = 0x80u;
        return true;
    case UI_BUTTON_REGION_OK:
        *btn_set1_out = 0x04u;
        return true;
    case UI_BUTTON_REGION_ESC:
        *btn_set1_out = 0x08u;
        return true;
    default:
        return false;
    }
}
