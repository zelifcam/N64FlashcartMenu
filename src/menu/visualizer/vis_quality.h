/**
 * @file vis_quality.h
 * @brief Adaptive quality scaling for visualizers
 * @ingroup visualizer
 *
 * Monitors frame time and provides a scale factor (0.2 – 1.5) that
 * visualizers use to reduce render complexity when the N64 is under load.
 *
 * VISUALIZER GUIDELINES:
 *   - Scale loop/segment/particle counts with vis_quality_scale_int(base).
 *   - Use vis_quality_get_scale() for a continuous factor.
 *   - Only scale render() — never reduce update() simulation fidelity.
 *   - vis_render() calls vis_quality_should_skip() automatically.
 */

#ifndef VIS_QUALITY_H__
#define VIS_QUALITY_H__

#include <stdbool.h>

typedef enum {
    QUALITY_LOW = 0,
    QUALITY_MEDIUM,
    QUALITY_HIGH,
    QUALITY_MAX,
} quality_level_t;

void            vis_quality_init(void);
void            vis_quality_update(void);
quality_level_t vis_quality_get_level(void);
float           vis_quality_get_scale(void);
int             vis_quality_scale_int(int base_max);
bool            vis_quality_should_skip(void);
float           vis_quality_get_frame_time(void);
void            vis_quality_force_up(void);
void            vis_quality_force_down(void);
void            vis_quality_force_reset(void);
bool            vis_quality_is_forced(void);

#endif /* VIS_QUALITY_H__ */
