/**
 * @file vis_quality.h
 * @brief Adaptive quality scaling for visualizers
 * @ingroup visualizer
 *
 * Monitors frame time and provides a scale factor (0.2 – 1.0) that
 * visualizers use to reduce render complexity when the N64 is under load.
 *
 * VISUALIZER GUIDELINES:
 *   - Scale loop/segment/particle counts with vis_quality_scale_int(base).
 *   - Use vis_quality_get_scale() for a continuous factor.
 *   - Only scale render() — never reduce update() simulation fidelity.
 */

#ifndef VIS_QUALITY_H__
#define VIS_QUALITY_H__

void  vis_quality_init(void);
void  vis_quality_update(void);
float vis_quality_get_scale(void);
int   vis_quality_scale_int(int base);

#endif /* VIS_QUALITY_H__ */
