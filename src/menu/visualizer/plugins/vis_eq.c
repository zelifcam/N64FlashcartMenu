/**
 * @file vis_eq.c
 * @brief 3D EQ visualizer — spectrum cylinders on an orbiting camera
 * @ingroup visualizer
 *
 * NUM_BARS cylinders mapped to FFT bands, colored bass-to-treble. A cinematic
 * camera orbits and breathes to the music. Formations morph between line,
 * circle, square, and double-line arrangements over time.
 */

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <math.h>
#include <stdlib.h>
#include "../visualizer.h"
#include "../fft.h"

/*===========================================================================
 * World constants
 *===========================================================================*/

/* EQ bars */
#define BAR_WIDTH        5.0f           /* XZ half-extent */
#define BAR_SPACING      14.0f          /* centre-to-centre */
#define BAR_MIN_HEIGHT   3.0f
#define BAR_MAX_HEIGHT   65.0f

/* Maximum bar count — compile-time array size.
 * Active bar count is controlled at runtime via D-up/D-down (4–64, steps of 4). */
#define NUM_BARS         64
#define BAR_COUNT_MIN    4
#define BAR_COUNT_MAX    64
#define BAR_COUNT_STEP   4
#define BAR_COUNT_DEFAULT 32

/* Cylinder sides — fixed */
#define BAR_SIDES        6

/* World */
#define WORLD_RADIUS     420.0f         /* camera orbit radius reference */
#define FLOOR_Y         -10.0f
#define CEIL_Y           110.0f

/* Camera */
#define CAM_NEAR         5.0f
#define CAM_FAR          600.0f
#define CAM_FOV          T3D_DEG_TO_RAD(70.0f)

/* Frustum cull tunables */
#define CULL_HEIGHT_BIAS  0.6f  /* fraction of cur_height added to behind-camera slack (accounts for tall bars) */

/* Formation morphing */
#define MORPH_SPEED      0.25f          /* lerp rate per second — slow, organic ease-out */
#define HOLD_LINE_MIN    20.0f          /* min seconds to hold line formation */
#define HOLD_LINE_MAX    35.0f          /* max seconds to hold line formation */
#define HOLD_OTHER_MIN   25.0f          /* min seconds to hold non-line formation */
#define HOLD_OTHER_MAX   45.0f          /* max seconds to hold non-line formation */

/* Circle formation radius */
#define CIRCLE_RADIUS    110.0f

/* Square: bars distributed evenly across 4 sides (uses total/4 at runtime) */
#define SQUARE_HALF      80.0f          /* half-extent of the square */

/* Double-line: two rows, bars distributed evenly (uses total/2 at runtime) */
#define DLINE_ROW_OFFSET 20.0f          /* Z offset from centre for each row */


/*===========================================================================
 * Types
 *===========================================================================*/

typedef enum {
    FORM_LINE        = 0,
    FORM_CIRCLE      = 1,
    FORM_SQUARE      = 2,
    FORM_DOUBLELINE  = 3,
    FORM_DIAMOND     = 4,
    FORM_STAR        = 5,
    FORM_SPIRAL      = 6,
    FORM_TRIANGLE    = 7,
    FORM_FIGURE8     = 8,
    FORM_LISSAJOUS   = 9,
    FORM_FLOWER      = 10,
    FORM_DOUBLERING  = 11,
    FORM_WAVE        = 12,
    FORM_GRID        = 13,
    FORM_TREFOIL     = 14,
    FORM_STARBURST   = 15,
    FORM_ARROW       = 16,
    FORM_PENTAGON    = 17,
    FORM_HYPOCYCLOID = 18,
    FORM_DNA         = 19,
    FORM_ZIGZAG      = 20,
    FORM_ELLIPSE     = 21,
    FORM_BUTTERFLY   = 22,
    FORM_COUNT       = 23,
} formation_t;

/* Number of framebuffers — must match display_init() call in menu.c */
#define FB_COUNT  2

typedef struct {
    int   sides;           /* polygon cross-section (3–6)           */
    float radius;          /* base XZ radius                        */
    float max_height;      /* height at band energy = 1.0           */
    float cur_height;      /* smoothed current height               */
    float pos_x, pos_z;    /* world position (lerped current)       */
    float tgt_x, tgt_z;   /* world position target for this frame  */
    uint32_t color;        /* RGBA packed vertex color              */
    int   band_lo;         /* first FFT band driving this object    */
    int   band_hi;         /* last  FFT band driving this object    */

    /* per-object geometry (allocated once at init) */
    T3DVertPacked *verts;
    T3DMat4FP     *matrix;         /* FB_COUNT matrices, indexed by frame_idx */
    rspq_block_t  *dpl;            /* pre-recorded draw commands            */
    int            vert_count;
    float          last_height[FB_COUNT]; /* height at last matrix rebuild, per buffer */
    float          last_px[FB_COUNT];     /* pos_x at last matrix rebuild, per buffer */
    float          last_pz[FB_COUNT];     /* pos_z at last matrix rebuild, per buffer */
} world_obj_t;


/*===========================================================================
 * Static state
 *===========================================================================*/

static world_obj_t  objects[NUM_BARS];
static int          obj_count  = 0;
static int          frame_idx  = 0;   /* cycles 0..FB_COUNT-1 each render */

static T3DViewport  viewport;


/* Smoothed audio — per-frame inputs to update functions */
static float s_bands[FFT_NUM_BANDS] = {0};  /* per-band spectrum, light smoothing */
static float s_star_bass = 0.0f;            /* ultra-slow bass drift for starfield */

/* Camera — continuous orbit, no cuts */
static T3DVec3 cam_eye;
static T3DVec3 cam_target;
static float   cam_angle = 0.0f;   /* orbit angle, accumulates forever */
static float   cam_time  = 0.0f;   /* absolute time for height breathing */
static float   cam_bass  = 0.0f;   /* phrase-level bass for orbit/look-at */
static float   beat_kick = 0.0f;   /* decaying beat impulse → orbit speed pulse */
static float   overhead_blend  = 0.0f;  /* 0=orbit, 1=straight-down overhead */
static float   overhead_timer  = 0.0f;  /* accumulates; triggers overhead window */
static float   overhead_period = 55.0f; /* randomised each cycle (45–75 s) */
#define OVERHEAD_HOLD  8.0f             /* seconds to hold the overhead view */

/* Formation state machine */
static formation_t form_current = FORM_LINE;
static float       form_hold_remaining = 0.0f; /* seconds until we transition */
static float       form_cam_scale = 1.0f;      /* smoothed camera radius scale: 1=full pullback, 0.45=tight */

/* Dynamic bar count — D-up/D-down steps through 4,8,...,64 */
static int   target_bar_count  = BAR_COUNT_DEFAULT;
static float display_bar_count = BAR_COUNT_DEFAULT; /* smoothed, drives obj_count */

/* Starfield background.
 * Array always holds STAR_COUNT_MAX stars; active draw count scales inversely
 * with bar count: 1 bar→128 stars, 64 bars→8 stars (linear interpolation). */
#define STAR_COUNT_MAX  128
#define STAR_COUNT_MIN  8
typedef struct { float x, y, z, speed; } star_t;
static star_t  stars[STAR_COUNT_MAX];
static int     star_draw_count = STAR_COUNT_MAX; /* updated each frame from obj_count */

/*===========================================================================
 * RNG (simple LCG — no stdlib rand dependency on N64)
 *===========================================================================*/

static uint32_t rng_state = 0;

static uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/* float in [lo, hi] */
static float rng_float(float lo, float hi) {
    return lo + ((float)(rng_next() & 0xFFFF) / 65535.0f) * (hi - lo);
}


/*===========================================================================
 * Formation layout computation
 *===========================================================================*/

/* Compute (x, z) world position for bar i in the given formation.
 * `total` is the current active bar count — all layout math scales from it. */
static void formation_pos(formation_t form, int i, int total, float *out_x, float *out_z) {
    if (total < 1) total = 1;
    switch (form) {
        case FORM_LINE: {
            float total_width = (total - 1) * BAR_SPACING;
            *out_x = -total_width * 0.5f + i * BAR_SPACING;
            *out_z = 0.0f;
            break;
        }
        case FORM_CIRCLE: {
            float a = (float)i / total * (2.0f * T3D_PI);
            *out_x = fm_cosf(a) * CIRCLE_RADIUS;
            *out_z = fm_sinf(a) * CIRCLE_RADIUS;
            break;
        }
        case FORM_SQUARE: {
            int sbs  = total / 4;
            if (sbs < 1) sbs = 1;
            int side = i / sbs;
            int pos  = i % sbs;
            float t  = (float)pos / sbs;
            float h  = SQUARE_HALF;
            switch (side) {
                case 0: *out_x = -h + t * 2.0f * h; *out_z = -h;               break;
                case 1: *out_x =  h;                 *out_z = -h + t * 2.0f * h; break;
                case 2: *out_x =  h - t * 2.0f * h; *out_z =  h;               break;
                case 3: *out_x = -h;                 *out_z =  h - t * 2.0f * h; break;
                default: *out_x = 0; *out_z = 0; break;
            }
            break;
        }
        case FORM_DOUBLELINE: {
            int row_bars = total / 2;
            if (row_bars < 1) row_bars = 1;
            int row = i / row_bars;
            int col = i % row_bars;
            float total_width = (row_bars - 1) * BAR_SPACING;
            *out_x = -total_width * 0.5f + col * BAR_SPACING;
            *out_z = (row == 0) ? -DLINE_ROW_OFFSET : DLINE_ROW_OFFSET;
            break;
        }
        case FORM_DIAMOND: {
            float corners[4][2] = {{0,-90},{90,0},{0,90},{-90,0}};
            int bps  = total / 4;
            if (bps < 1) bps = 1;
            int side = i / bps, pos = i % bps;
            float t  = (float)pos / (float)bps;
            int next = (side + 1) % 4;
            *out_x = corners[side][0] + t * (corners[next][0] - corners[side][0]);
            *out_z = corners[side][1] + t * (corners[next][1] - corners[side][1]);
            break;
        }
        case FORM_STAR: {
            float r_outer = 100.0f, r_inner = 50.0f;
            float a = (float)i / (float)total * (2.0f * T3D_PI);
            float r = (i % 2 == 0) ? r_outer : r_inner;
            *out_x = fm_cosf(a) * r;
            *out_z = fm_sinf(a) * r;
            break;
        }
        case FORM_SPIRAL: {
            float t  = (total > 1) ? (float)i / (total - 1) : 0.0f;
            float a  = t * 2.75f * T3D_PI;
            float r  = 20.0f + t * 80.0f;
            *out_x = fm_cosf(a) * r;
            *out_z = fm_sinf(a) * r;
            break;
        }
        case FORM_TRIANGLE: {
            float r = 95.0f;
            float corners[3][2] = {
                { fm_cosf(T3D_PI*0.5f)*r,   fm_sinf(T3D_PI*0.5f)*r   },
                { fm_cosf(T3D_PI*1.167f)*r, fm_sinf(T3D_PI*1.167f)*r },
                { fm_cosf(T3D_PI*1.833f)*r, fm_sinf(T3D_PI*1.833f)*r },
            };
            int base_len = total / 3;
            int extra    = total % 3;
            int seg = 0, seg_start = 0;
            for (int s = 0; s < 3; s++) {
                int slen = base_len + (s < extra ? 1 : 0);
                if (i < seg_start + slen) { seg = s; break; }
                seg_start += slen;
            }
            int seg_len = base_len + (seg < extra ? 1 : 0);
            if (seg_len < 1) seg_len = 1;
            float t = (float)(i - seg_start) / (float)seg_len;
            int next = (seg + 1) % 3;
            *out_x = corners[seg][0] + t * (corners[next][0] - corners[seg][0]);
            *out_z = corners[seg][1] + t * (corners[next][1] - corners[seg][1]);
            break;
        }
        case FORM_FIGURE8: {
            int half = total / 2;
            if (half < 1) half = 1;
            float r = 55.0f, offset = 60.0f;
            if (i < half) {
                float a = (float)i / (float)half * (2.0f * T3D_PI);
                *out_x = fm_cosf(a) * r;
                *out_z = -offset + fm_sinf(a) * r;
            } else {
                float a = (float)(i - half) / (float)half * (2.0f * T3D_PI);
                *out_x = fm_cosf(a) * r;
                *out_z =  offset + fm_sinf(a) * r;
            }
            break;
        }
        case FORM_LISSAJOUS: {
            float t = (float)i / total * (2.0f * T3D_PI);
            *out_x = 80.0f * fm_sinf(3.0f * t + T3D_PI * 0.25f);
            *out_z = 80.0f * fm_sinf(2.0f * t);
            break;
        }
        case FORM_FLOWER: {
            float t = (float)i / total * (2.0f * T3D_PI);
            float r = fabsf(fm_cosf(4.0f * t)) * 90.0f;
            *out_x = fm_cosf(t) * r;
            *out_z = fm_sinf(t) * r;
            break;
        }
        case FORM_DOUBLERING: {
            int half = total / 2;
            if (half < 1) half = 1;
            if (i < half) {
                float a = (float)i / (float)half * (2.0f * T3D_PI);
                *out_x = fm_cosf(a) * 55.0f;
                *out_z = fm_sinf(a) * 55.0f;
            } else {
                float a = (float)(i - half) / (float)half * (2.0f * T3D_PI);
                *out_x = fm_cosf(a) * 100.0f;
                *out_z = fm_sinf(a) * 100.0f;
            }
            break;
        }
        case FORM_WAVE: {
            float t = (total > 1) ? (float)i / (total - 1) : 0.0f;
            *out_x = -150.0f + t * 300.0f;
            *out_z = 60.0f * fm_sinf(t * 2.0f * T3D_PI * 2.5f);
            break;
        }
        case FORM_GRID: {
            int cols = total / 4;
            if (cols < 1) cols = 1;
            int col = i % cols, row = i / cols;
            float spacing = 15.0f;
            *out_x = -(cols - 1) * spacing * 0.5f + col * spacing;
            *out_z =  -1.5f * spacing + row * spacing;
            break;
        }
        case FORM_TREFOIL: {
            float t = (float)i / total * (2.0f * T3D_PI);
            *out_x = (fm_sinf(t) + 2.0f * fm_sinf(2.0f * t)) * 28.0f;
            *out_z = (fm_cosf(t) - 2.0f * fm_cosf(2.0f * t)) * 28.0f;
            break;
        }
        case FORM_STARBURST: {
            int spokes = total / 2;
            if (spokes < 1) spokes = 1;
            float a = (float)(i / 2) / (float)spokes * (2.0f * T3D_PI);
            float r = (i % 2 == 0) ? 40.0f : 95.0f;
            *out_x = fm_cosf(a) * r;
            *out_z = fm_sinf(a) * r;
            break;
        }
        case FORM_ARROW: {
            int shaft = total * 5 / 8;
            if (shaft < 1) shaft = 1;
            int head  = total - shaft;
            int half_head = head / 2;
            if (half_head < 1) half_head = 1;
            if (i < shaft) {
                float t = (shaft > 1) ? (float)i / (float)(shaft - 1) : 0.0f;
                *out_x = -100.0f + t * 130.0f;
                *out_z = 0.0f;
            } else {
                int hi = i - shaft;
                if (hi < half_head) {
                    float t = (half_head > 1) ? (float)hi / (float)(half_head - 1) : 0.0f;
                    *out_x = 30.0f + t * 60.0f;
                    *out_z = 50.0f * (1.0f - t);
                } else {
                    int rem = head - half_head;
                    float t = (rem > 1) ? (float)(hi - half_head) / (float)(rem - 1) : 0.0f;
                    *out_x = 90.0f - t * 60.0f;
                    *out_z = -(50.0f * t);
                }
            }
            break;
        }
        case FORM_PENTAGON: {
            float r = 90.0f;
            float pbase = T3D_PI * 0.5f;
            float corners[5][2];
            for (int k = 0; k < 5; k++) {
                float a = pbase + k * 2.0f * T3D_PI / 5.0f;
                corners[k][0] = fm_cosf(a) * r;
                corners[k][1] = fm_sinf(a) * r;
            }
            int base_len = total / 5;
            int extra    = total % 5;
            int seg = 0, seg_start = 0;
            for (int s = 0; s < 5; s++) {
                int slen = base_len + (s < extra ? 1 : 0);
                if (i < seg_start + slen) { seg = s; break; }
                seg_start += slen;
            }
            int seg_len = base_len + (seg < extra ? 1 : 0);
            if (seg_len < 1) seg_len = 1;
            float t = (float)(i - seg_start) / (float)seg_len;
            int next = (seg + 1) % 5;
            *out_x = corners[seg][0] + t * (corners[next][0] - corners[seg][0]);
            *out_z = corners[seg][1] + t * (corners[next][1] - corners[seg][1]);
            break;
        }
        case FORM_HYPOCYCLOID: {
            float t = (float)i / total * (2.0f * T3D_PI);
            float ct = fm_cosf(t), st = fm_sinf(t);
            *out_x = 90.0f * ct * ct * ct;
            *out_z = 90.0f * st * st * st;
            break;
        }
        case FORM_DNA: {
            float total_w = 280.0f;
            int strand = i % 2;
            int idx    = i / 2;
            int pairs  = total / 2;
            if (pairs < 1) pairs = 1;
            float t = (pairs > 1) ? (float)idx / (float)(pairs - 1) : 0.0f;
            float phase = strand * T3D_PI;
            *out_x = -total_w * 0.5f + t * total_w;
            *out_z = 55.0f * fm_sinf(t * 2.0f * T3D_PI * 1.5f + phase);
            break;
        }
        case FORM_ZIGZAG: {
            float t = (total > 1) ? (float)i / (total - 1) : 0.0f;
            *out_x = -150.0f + t * 300.0f;
            float phase = t * (float)(total / 2);
            float frac = phase - (int)phase;
            *out_z = 70.0f * (frac < 0.5f ? 4.0f * frac - 1.0f : 3.0f - 4.0f * frac);
            break;
        }
        case FORM_ELLIPSE: {
            float a = (float)i / total * (2.0f * T3D_PI);
            *out_x = fm_cosf(a) * 130.0f;
            *out_z = fm_sinf(a) *  60.0f;
            break;
        }
        case FORM_BUTTERFLY: {
            float t = (float)i / total * (2.0f * T3D_PI);
            float r = fabsf(fm_cosf(2.0f * t)) * 90.0f;
            *out_x = fm_cosf(t) * r;
            *out_z = fm_sinf(t) * r;
            break;
        }
        default:
            *out_x = 0; *out_z = 0;
            break;
    }
}

/* Pick the next formation.
 * When on line: always pick a non-line formation at random.
 * When on a non-line: 70% chance to return to line, 30% to pick another non-line. */
static formation_t formation_pick_next(void) {
    formation_t candidates[FORM_COUNT];
    int nc = 0;

    if (form_current != FORM_LINE) {
        /* Strong pull back to line */
        int roll = (int)(rng_next() % 10u);
        if (roll < 7) return FORM_LINE;
        /* Remaining 30%: pick a different non-line formation */
        for (int f = 0; f < FORM_COUNT; f++) {
            if (f != (int)form_current && f != (int)FORM_LINE)
                candidates[nc++] = (formation_t)f;
        }
        if (nc == 0) return FORM_LINE;
        return candidates[(int)(rng_next() % (uint32_t)nc)];
    }

    /* Currently on line: pick any non-line formation */
    for (int f = 0; f < FORM_COUNT; f++) {
        if (f != (int)FORM_LINE) candidates[nc++] = (formation_t)f;
    }
    if (nc == 0) return FORM_LINE;
    return candidates[(int)(rng_next() % (uint32_t)nc)];
}

/*
 * Optimal-transport assignment for formation transitions.
 *
 * Naive index matching (bar[i] → target[i]) causes visible crossing because
 * the target formation's index order has no relation to the bars' current
 * spatial order.  The monotone rearrangement theorem (1D optimal transport)
 * gives the unique minimum-cost, non-crossing assignment: sort both the
 * current positions and the target positions by the same key and match by rank.
 *
 * Sort key: X primary, Z tiebreak.  This guarantees no X-axis crossings
 * during the linear morph — bars on the left of the current formation always
 * move toward the left side of the target, never crossing a bar to their right.
 * Insertion sort is used because N ≤ 64 (one-time cost, ≤ 2048 comparisons).
 */
static void formation_set_targets(formation_t form) {
    /* Compute raw target positions in formation index order */
    float raw_x[NUM_BARS], raw_z[NUM_BARS];
    for (int i = 0; i < obj_count; i++)
        formation_pos(form, i, obj_count, &raw_x[i], &raw_z[i]);

    /* Sort current bar indices by (pos_x, pos_z) */
    int ci[NUM_BARS];
    for (int i = 0; i < obj_count; i++) ci[i] = i;
    for (int i = 1; i < obj_count; i++) {
        int k = ci[i], j = i - 1;
        while (j >= 0 && (objects[ci[j]].pos_x > objects[k].pos_x ||
                          (objects[ci[j]].pos_x == objects[k].pos_x &&
                           objects[ci[j]].pos_z > objects[k].pos_z)))
            { ci[j + 1] = ci[j]; j--; }
        ci[j + 1] = k;
    }

    /* Sort target indices by (raw_x, raw_z) */
    int ti[NUM_BARS];
    for (int i = 0; i < obj_count; i++) ti[i] = i;
    for (int i = 1; i < obj_count; i++) {
        int k = ti[i], j = i - 1;
        while (j >= 0 && (raw_x[ti[j]] > raw_x[k] ||
                          (raw_x[ti[j]] == raw_x[k] && raw_z[ti[j]] > raw_z[k])))
            { ti[j + 1] = ti[j]; j--; }
        ti[j + 1] = k;
    }

    /* Assign: bar at current rank i goes to target at rank i — no X crossings */
    for (int i = 0; i < obj_count; i++) {
        objects[ci[i]].tgt_x = raw_x[ti[i]];
        objects[ci[i]].tgt_z = raw_z[ti[i]];
    }
}

/* Advance the lerp and state machine each frame */
static void formation_update(float dt) {
    /* Lerp all bar positions toward their targets.
     * MORPH_SPEED is the fraction of remaining distance closed per second,
     * giving a smooth ease-out (exponential approach). */
    float alpha = MORPH_SPEED * dt;
    if (alpha > 1.0f) alpha = 1.0f;
    for (int i = 0; i < obj_count; i++) {
        world_obj_t *o = &objects[i];
        o->pos_x += (o->tgt_x - o->pos_x) * alpha;
        o->pos_z += (o->tgt_z - o->pos_z) * alpha;
    }

    /* Hold timer — count down, then pick a new formation */
    form_hold_remaining -= dt;
    if (form_hold_remaining <= 0.0f) {
        formation_t next = formation_pick_next();
        form_current = next;
        formation_set_targets(next);

        if (next == FORM_LINE)
            form_hold_remaining = rng_float(HOLD_LINE_MIN, HOLD_LINE_MAX);
        else
            form_hold_remaining = rng_float(HOLD_OTHER_MIN, HOLD_OTHER_MAX);
    }

    /* Camera radius target: line needs full pullback (1.0), compact shapes
     * stay much closer (0.45) so they fill the frame instead of looking tiny.
     * Smooth lerp so transitions are gradual. */
    float scale_target = (form_current == FORM_LINE) ? 1.0f : 0.45f;
    form_cam_scale += (scale_target - form_cam_scale) * (dt * 0.4f);
}


/*===========================================================================
 * Geometry helpers (ported from bars.c)
 *===========================================================================*/

static inline void set_vert(T3DVertPacked *verts, int vi,
                             int16_t x, int16_t y, int16_t z,
                             uint16_t norm, uint32_t color) {
    int idx = vi / 2;
    if (vi % 2 == 0) {
        verts[idx].posA[0] = x;  verts[idx].posA[1] = y;  verts[idx].posA[2] = z;
        verts[idx].normA   = norm;
        verts[idx].rgbaA   = color;
        verts[idx].stA[0]  = 0;  verts[idx].stA[1] = 0;
    } else {
        verts[idx].posB[0] = x;  verts[idx].posB[1] = y;  verts[idx].posB[2] = z;
        verts[idx].normB   = norm;
        verts[idx].rgbaB   = color;
        verts[idx].stB[0]  = 0;  verts[idx].stB[1] = 0;
    }
}

/**
 * Build a bar (S-sided cylinder) — sides + top cap only (no bottom cap).
 * The bottom faces the floor and is never visible from any camera angle
 * used by this visualizer, so omitting it saves S triangles per bar.
 *
 * Vertex layout:
 *   [0..S-1]   top ring    (body color, outward norm)
 *   [S..2S-1]  bottom ring (darker shade, outward norm)
 *   [2S]       top cap centre (up norm)
 *
 * Winding: CCW when viewed from outside.
 * No zbuffer — bars drawn in back-to-front painter's order.
 * Positions in unit space (radius=64, height 0->64).
 * Matrix scales to actual radius/height at draw time.
 */
static bool build_bar(world_obj_t *obj) {
    int S = obj->sides;
    /* S top ring + S bottom ring + 1 top cap centre (no bottom cap) */
    obj->vert_count = S * 2 + 1;
    int packed = (obj->vert_count + 1) / 2;

    obj->verts  = malloc_uncached(sizeof(T3DVertPacked) * packed);
    obj->matrix = malloc_uncached(sizeof(T3DMat4FP) * FB_COUNT);
    if (!obj->verts || !obj->matrix) return false;

    uint32_t col = obj->color;
    /* Darker shade for bottom ring — gives a subtle gradient that grounds the bar */
    uint8_t r = (col >> 24) & 0xFF;
    uint8_t g = (col >> 16) & 0xFF;
    uint8_t b = (col >>  8) & 0xFF;
    uint32_t col_dark = ((uint32_t)(r * 35 / 100) << 24)
                      | ((uint32_t)(g * 35 / 100) << 16)
                      | ((uint32_t)(b * 35 / 100) <<  8)
                      | 0xFF;
    int vi = 0;

    uint16_t norm_up = t3d_vert_pack_normal(&(T3DVec3){{0, 1, 0}});

    /* [0..S-1] Top ring */
    for (int i = 0; i < S; i++) {
        float a = (float)i / S * (2.0f * T3D_PI);
        float cx = fm_cosf(a), cz = fm_sinf(a);
        uint16_t norm = t3d_vert_pack_normal(&(T3DVec3){{cx, 0.3f, cz}});
        set_vert(obj->verts, vi++, (int16_t)(cx*64), 64, (int16_t)(cz*64), norm, col);
    }
    /* [S..2S-1] Bottom ring — darker shade for grounding gradient */
    for (int i = 0; i < S; i++) {
        float a = (float)i / S * (2.0f * T3D_PI);
        float cx = fm_cosf(a), cz = fm_sinf(a);
        uint16_t norm = t3d_vert_pack_normal(&(T3DVec3){{cx, -0.3f, cz}});
        set_vert(obj->verts, vi++, (int16_t)(cx*64), 0, (int16_t)(cz*64), norm, col_dark);
    }
    /* [2S] Top cap centre */
    set_vert(obj->verts, vi++, 0, 64, 0, norm_up, col);

    data_cache_hit_writeback(obj->verts, sizeof(T3DVertPacked) * packed);

    int tc = S * 2;   /* top cap centre index */

    rspq_block_begin();
    t3d_vert_load(obj->verts, 0, obj->vert_count);

    /* Side faces — CCW from outside */
    for (int i = 0; i < S; i++) {
        int t0 = i,         t1 = (i + 1) % S;
        int b0 = S + i,     b1 = S + (i + 1) % S;
        t3d_tri_draw(t0, t1, b0);
        t3d_tri_draw(t1, b1, b0);
    }
    /* Top cap — CCW from above */
    for (int i = 0; i < S; i++)
        t3d_tri_draw(tc, i, (i + 1) % S);
    t3d_tri_sync();

    obj->dpl = rspq_block_end();

    for (int f = 0; f < FB_COUNT; f++) {
        obj->last_height[f] = -1.0f;
        obj->last_px[f]     = 1e10f;
        obj->last_pz[f]     = 1e10f;
    }
    return true;
}

/* Rewrite vertex colors in an already-built bar buffer without rebuilding geometry.
 * Called when obj->color changes so the spectrum shift is visible immediately. */
static void update_bar_color(world_obj_t *obj) {
    int S = obj->sides;
    uint32_t col = obj->color;
    uint8_t r = (col >> 24) & 0xFF;
    uint8_t g = (col >> 16) & 0xFF;
    uint8_t b = (col >>  8) & 0xFF;
    uint32_t col_dark = ((uint32_t)(r * 35 / 100) << 24)
                      | ((uint32_t)(g * 35 / 100) << 16)
                      | ((uint32_t)(b * 35 / 100) <<  8)
                      | 0xFF;

    /* [0..S-1] top ring — full color */
    for (int i = 0; i < S; i++) {
        int idx = i / 2;
        if (i % 2 == 0) obj->verts[idx].rgbaA = col;
        else             obj->verts[idx].rgbaB = col;
    }
    /* [S..2S-1] bottom ring — darker shade */
    for (int i = 0; i < S; i++) {
        int vi  = S + i;
        int idx = vi / 2;
        if (vi % 2 == 0) obj->verts[idx].rgbaA = col_dark;
        else              obj->verts[idx].rgbaB = col_dark;
    }
    /* [2S] top cap centre — full color */
    {
        int vi  = S * 2;
        int idx = vi / 2;
        if (vi % 2 == 0) obj->verts[idx].rgbaA = col;
        else              obj->verts[idx].rgbaB = col;
    }

    int packed = (obj->vert_count + 1) / 2;
    data_cache_hit_writeback(obj->verts, sizeof(T3DVertPacked) * packed);
}

static void draw_object(world_obj_t *obj) {
    if (!obj->verts || !obj->matrix || !obj->dpl) return;

    T3DMat4FP *mat = &obj->matrix[frame_idx];

    /* Rebuild matrix when height or XZ position changed meaningfully.
     * Height: tight threshold (0.5 units) so bar animation stays smooth.
     * Position: adaptive threshold — tight (0.5) when actively morphing (bar
     * is far from target), loose (2.0) when settled (close to target).
     * This gives smooth morph transitions without per-frame rebuilds at rest. */
    float lh = obj->last_height[frame_idx];
    float lpx = obj->last_px[frame_idx];
    float lpz = obj->last_pz[frame_idx];
    bool height_changed = (lh < 0.0f || fabsf(obj->cur_height - lh) > 0.5f);
    float dist_to_tgt = (obj->tgt_x - obj->pos_x) * (obj->tgt_x - obj->pos_x)
                      + (obj->tgt_z - obj->pos_z) * (obj->tgt_z - obj->pos_z);
    float pos_thresh = (dist_to_tgt > 25.0f) ? 0.5f : 2.0f;
    bool pos_changed = (fabsf(obj->pos_x - lpx) > pos_thresh ||
                        fabsf(obj->pos_z - lpz) > pos_thresh);

    if (height_changed || pos_changed) {
        float scale_xz = obj->radius / 64.0f;
        float scale_y  = obj->cur_height / 64.0f;
        t3d_mat4fp_identity(mat);
        t3d_mat4fp_set_float(mat, 0, 0, scale_xz);
        t3d_mat4fp_set_float(mat, 1, 1, scale_y);
        t3d_mat4fp_set_float(mat, 2, 2, scale_xz);
        t3d_mat4fp_set_float(mat, 3, 0, obj->pos_x);
        t3d_mat4fp_set_float(mat, 3, 1, FLOOR_Y);
        t3d_mat4fp_set_float(mat, 3, 2, obj->pos_z);
        obj->last_height[frame_idx] = obj->cur_height;
        obj->last_px[frame_idx]     = obj->pos_x;
        obj->last_pz[frame_idx]     = obj->pos_z;
    }

    t3d_matrix_push(mat);
    rspq_block_run(obj->dpl);
    t3d_matrix_pop(1);
}


/*===========================================================================
 * Camera — continuous orbit, never cuts
 *===========================================================================*/

static void camera_init(void) {
    cam_angle = rng_float(0.0f, 2.0f * T3D_PI);
    cam_time  = 0.0f;
    cam_bass       = 0.0f;
    beat_kick      = 0.0f;
    overhead_blend = 0.0f;
    overhead_timer = 0.0f;
    overhead_period = 45.0f + rng_float(0.0f, 30.0f);
    cam_eye    = (T3DVec3){{ fm_cosf(cam_angle) * WORLD_RADIUS,
                             CEIL_Y * 0.5f,
                             fm_sinf(cam_angle) * WORLD_RADIUS }};
    cam_target = (T3DVec3){{0, BAR_MAX_HEIGHT * 0.4f, 0}};
}

static void camera_update(const vis_audio_t *audio) {
    cam_time += audio->dt;

    /* Phrase-level bass smoothing for orbit and look-at */
    cam_bass = cam_bass * 0.80f + audio->bass * 0.20f;

    /* Beat kick — instant set on each beat, ~15-frame exponential decay */
    if (audio->beat && audio->beat_intensity > beat_kick)
        beat_kick = audio->beat_intensity;
    beat_kick *= 0.88f;

    /* Overhead blend — driven by periodic timer and by fps < 18 */
    float fps = (audio->dt > 0.001f) ? 1.0f / audio->dt : 18.0f;
    overhead_timer += audio->dt;
    float overhead_target = (fps < 18.0f ||
                             overhead_timer > overhead_period - OVERHEAD_HOLD) ? 1.0f : 0.0f;
    if (overhead_timer >= overhead_period) {
        overhead_timer  = 0.0f;
        overhead_period = 45.0f + rng_float(0.0f, 30.0f);
    }
    float blend_rate = (overhead_target > overhead_blend) ? 2.0f : 0.8f;
    overhead_blend += (overhead_target - overhead_blend) * blend_rate * audio->dt;
    if (overhead_blend < 0.0f) overhead_blend = 0.0f;
    if (overhead_blend > 1.0f) overhead_blend = 1.0f;

    /* Orbit — bass nudges base speed; beat kick adds a momentary spin pulse */
    cam_angle += (0.10f + cam_bass * 0.08f + beat_kick * 0.04f) * audio->dt;

    /* Height — two incommensurate sines keep the view always changing */
    float h_raw = 0.72f
        + 0.20f * fm_sinf(cam_time * 0.0785f)   /* ~80s period */
        + 0.12f * fm_sinf(cam_time * 0.273f);   /* ~23s period */
    if (h_raw < 0.0f) h_raw = 0.0f;
    if (h_raw > 1.0f) h_raw = 1.0f;
    float h_norm = h_raw * h_raw * (3.0f - 2.0f * h_raw); /* smoothstep */

    /* Orbit camera position */
    float bar_scale = (form_current == FORM_LINE)
        ? (float)obj_count / (float)BAR_COUNT_DEFAULT : 1.0f;
    float r = WORLD_RADIUS * bar_scale
        * (1.0f - 0.35f * h_norm)
        * form_cam_scale
        * (1.0f + 0.07f * fm_sinf(cam_time * 0.11f));

    float orbit_x  = fm_cosf(cam_angle) * r;
    float orbit_y  = FLOOR_Y + BAR_MAX_HEIGHT * 2.8f * h_norm;
    float orbit_z  = fm_sinf(cam_angle) * r;
    float orbit_ty = FLOOR_Y + BAR_MAX_HEIGHT * (0.5f - 0.5f * h_norm + cam_bass * 0.15f);

    /* Overhead position — tiny lateral offset avoids degenerate up vector */
    float ov_x  = 18.0f;
    float ov_y  = FLOOR_Y + BAR_MAX_HEIGHT * 7.0f;
    float ov_z  = 0.0f;
    float ov_ty = FLOOR_Y;

    /* Lerp orbit ↔ overhead */
    float ob = overhead_blend;
    cam_eye.v[0] = orbit_x  + (ov_x  - orbit_x)  * ob;
    cam_eye.v[1] = orbit_y  + (ov_y  - orbit_y)  * ob;
    cam_eye.v[2] = orbit_z  + (ov_z  - orbit_z)  * ob;

    cam_target = (T3DVec3){{ 0.0f,
        orbit_ty + (ov_ty - orbit_ty) * ob,
        0.0f }};
}

/*===========================================================================
 * World roll
 *===========================================================================*/

static void free_objects(void) {
    for (int i = 0; i < obj_count; i++) {
        if (objects[i].dpl)    { rspq_block_free(objects[i].dpl);  objects[i].dpl    = NULL; }
        if (objects[i].verts)  { free_uncached(objects[i].verts);  objects[i].verts  = NULL; }
        if (objects[i].matrix) { free_uncached(objects[i].matrix); objects[i].matrix = NULL; }
    }
}

static void world_roll(void) {
    rng_state = (uint32_t)TICKS_READ();
    frame_idx = 0;

    free_objects();

    /* Always allocate all NUM_BARS — active count is controlled at runtime */
    obj_count          = BAR_COUNT_DEFAULT;
    target_bar_count   = BAR_COUNT_DEFAULT;
    display_bar_count  = BAR_COUNT_DEFAULT;

    for (int i = 0; i < NUM_BARS; i++) {
        world_obj_t *o = &objects[i];

        o->sides      = BAR_SIDES;
        o->radius     = BAR_WIDTH;
        o->max_height = BAR_MAX_HEIGHT;
        o->cur_height = BAR_MIN_HEIGHT;

        /* Assign band range and color spread across active slots only */
        int n = obj_count;
        o->band_lo = (i * FFT_NUM_BANDS) / n;
        o->band_hi = ((i + 1) * FFT_NUM_BANDS) / n - 1;
        if (o->band_hi < o->band_lo) o->band_hi = o->band_lo;
        if (o->band_hi >= FFT_NUM_BANDS) o->band_hi = FFT_NUM_BANDS - 1;

        /* Spectrum color: bass(red) → mid(green) → treble(blue) */
        float ct = (n > 1) ? (float)i / (n - 1) : 0.0f;
        uint8_t cr = (uint8_t)((1.0f - ct) * 255);
        uint8_t cg = (uint8_t)(fm_sinf(ct * T3D_PI) * 220);
        uint8_t cb = (uint8_t)(ct * 255);
        o->color = ((uint32_t)cr << 24) | ((uint32_t)cg << 16) | ((uint32_t)cb << 8) | 0xFF;

        formation_pos(FORM_LINE, i, BAR_COUNT_DEFAULT, &o->pos_x, &o->pos_z);
        o->tgt_x = o->pos_x;
        o->tgt_z = o->pos_z;

        build_bar(o);
    }

    /* Start in line formation, hold for a good while */
    form_current        = FORM_LINE;
    form_hold_remaining = rng_float(HOLD_LINE_MIN, HOLD_LINE_MAX);
    form_cam_scale      = 1.0f;
    formation_set_targets(FORM_LINE);
}

/*===========================================================================
 * Starfield background
 *===========================================================================*/

/* Screen dimensions for the visualizer — 640x480 native display */
#define VIS_W  640.0f
#define VIS_H  480.0f
#define VIS_CX (VIS_W * 0.5f)
#define VIS_CY (VIS_H * 0.5f)

static void starfield_init(void) {
    for (int i = 0; i < STAR_COUNT_MAX; i++) {
        stars[i].x     = (float)(rng_next() % (int)VIS_W) - VIS_CX;
        stars[i].y     = (float)(rng_next() % (int)VIS_H) - VIS_CY;
        stars[i].z     = (float)(rng_next() % 500) + 100.0f;
        stars[i].speed = 12.0f + (float)(rng_next() % 18);
    }
    star_draw_count = STAR_COUNT_MAX;
}

static void starfield_update(float dt) {
    for (int i = 0; i < star_draw_count; i++) {
        stars[i].z -= stars[i].speed * dt;
        if (stars[i].z < 1.0f) {
            stars[i].x     = (float)(rng_next() % (int)VIS_W) - VIS_CX;
            stars[i].y     = (float)(rng_next() % (int)VIS_H) - VIS_CY;
            stars[i].z     = 500.0f + (float)(rng_next() % 100);
            stars[i].speed = 12.0f + (float)(rng_next() % 18);
        }
    }
}

static void starfield_draw(float bass) {
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);

    float bass_scale = 0.55f + bass * 0.45f;
    for (int bucket = 3; bucket >= 0; bucket--) {
        float lo = bucket * 0.25f;
        float hi = lo + 0.25f;
        float mid_bright = (lo + hi) * 0.5f * bass_scale;
        if (mid_bright > 1.0f) mid_bright = 1.0f;
        uint8_t c = (uint8_t)(255 * mid_bright);
        rdpq_set_prim_color(RGBA32(c, c, c, 255));

        for (int i = 0; i < star_draw_count; i++) {
            float inv_z = 400.0f / stars[i].z;
            float sx = VIS_CX + stars[i].x * inv_z;
            float sy = VIS_CY + stars[i].y * inv_z;
            if (sx < 0 || sx > VIS_W || sy < 0 || sy > VIS_H) continue;
            float bright = 1.0f - stars[i].z / 600.0f;
            if (bright < lo || bright >= hi) continue;
            float size = 1.5f + bright * 2.5f;
            rdpq_fill_rectangle(sx - size * 0.5f, sy - size * 0.5f,
                                sx + size * 0.5f, sy + size * 0.5f);
        }
    }
}

/*===========================================================================
 * Visualizer callbacks
 *===========================================================================*/

static void world_init(void) {
    viewport = t3d_viewport_create_buffered(FB_COUNT);
    t3d_viewport_set_projection(&viewport, CAM_FOV, CAM_NEAR, CAM_FAR);
    world_roll();
    camera_init();
    starfield_init();

    /* Lighting is static — set once here rather than every frame */
    t3d_light_set_ambient((uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF});
    t3d_light_set_count(0);
}

static void world_cleanup(void) {
    free_objects();
    obj_count = 0;
    t3d_viewport_destroy(&viewport);
}

static void world_update(const vis_audio_t *audio) {
    /* Star bass: ultra-slow drift, no beat-sync jerk */
    s_star_bass = s_star_bass * 0.995f + audio->bass * 0.005f;
    /* Per-band smoothing — fast enough to feel live, enough to kill single-frame spikes */
    for (int i = 0; i < FFT_NUM_BANDS; i++)
        s_bands[i] = s_bands[i] * 0.5f + audio->bands[i] * 0.5f;

    /* D-up / D-down: step bar count through 4,8,...,64 */
    joypad_buttons_t pressed;
    pressed.raw = audio->buttons_pressed;
    if (pressed.d_up && target_bar_count < BAR_COUNT_MAX)
        target_bar_count += BAR_COUNT_STEP;
    if (pressed.d_down && target_bar_count > BAR_COUNT_MIN)
        target_bar_count -= BAR_COUNT_STEP;

    /* Smoothly lerp display_bar_count toward target (~6 bars/sec) */
    float bar_diff = (float)target_bar_count - display_bar_count;
    display_bar_count += bar_diff * (audio->dt * 6.0f);
    if (display_bar_count < BAR_COUNT_MIN) display_bar_count = BAR_COUNT_MIN;
    if (display_bar_count > BAR_COUNT_MAX) display_bar_count = BAR_COUNT_MAX;
    int new_count = (int)(display_bar_count + 0.5f);

    /* When active count changes, retarget formations and reassign bands/colors */
    if (new_count != obj_count) {
        obj_count = new_count;
        formation_set_targets(form_current);

        for (int i = 0; i < obj_count; i++) {
            world_obj_t *o = &objects[i];
            o->band_lo = (i * FFT_NUM_BANDS) / obj_count;
            o->band_hi = ((i + 1) * FFT_NUM_BANDS) / obj_count - 1;
            if (o->band_hi < o->band_lo) o->band_hi = o->band_lo;
            if (o->band_hi >= FFT_NUM_BANDS) o->band_hi = FFT_NUM_BANDS - 1;

            float t = (obj_count > 1) ? (float)i / (obj_count - 1) : 0.0f;
            uint32_t new_color =
                ((uint32_t)((uint8_t)((1.0f - t) * 255)) << 24) |
                ((uint32_t)((uint8_t)(fm_sinf(t * T3D_PI) * 220)) << 16) |
                ((uint32_t)((uint8_t)(t * 255)) << 8) | 0xFF;
            if (new_color != o->color) {
                o->color = new_color;
                if (o->verts) update_bar_color(o);
            }
        }
    }

    /* Update active bars from their assigned band range */
    float decay = 1.0f - 22.0f * audio->dt;
    if (decay < 0.0f) decay = 0.0f;

    for (int i = 0; i < obj_count; i++) {
        world_obj_t *o = &objects[i];

        float energy = 0.0f;
        for (int b = o->band_lo; b <= o->band_hi; b++) {
            if (s_bands[b] > energy) energy = s_bands[b];
        }

        float target = BAR_MIN_HEIGHT + energy * (o->max_height - BAR_MIN_HEIGHT);
        /* Instant attack, fast exponential decay (~22x per second) */
        o->cur_height = (target > o->cur_height)
            ? target
            : (o->cur_height - target) * decay + target;
    }

    /* Star count scales inversely with bar count: more bars = fewer stars.
     * Linear interpolation: 1 bar→128 stars, 64 bars→8 stars. */
    {
        float t = (float)(obj_count - 1) / (float)(BAR_COUNT_MAX - 1);
        int sc = (int)(STAR_COUNT_MAX + t * (STAR_COUNT_MIN - STAR_COUNT_MAX) + 0.5f);
        if (sc < STAR_COUNT_MIN) sc = STAR_COUNT_MIN;
        if (sc > STAR_COUNT_MAX) sc = STAR_COUNT_MAX;
        star_draw_count = sc;
    }

    /* Shrink inactive bars smoothly to the floor so they disappear gracefully */
    for (int i = obj_count; i < NUM_BARS; i++) {
        world_obj_t *o = &objects[i];
        o->cur_height = o->cur_height * 0.85f + BAR_MIN_HEIGHT * 0.15f;
    }

    formation_update(audio->dt);
    camera_update(audio);
    starfield_update(audio->dt);
}

/* Returns false if the bar is completely behind the camera and safe to skip.
 * This is the only cull that is guaranteed never to cause visible popping —
 * a bar truly behind the camera is 100% invisible regardless of its height.
 *
 * The slack term accounts for tall bars: the camera could be positioned beside
 * a bar and looking slightly past it, so the tip is still in frame even though
 * the base is behind the forward plane.  CULL_HEIGHT_BIAS controls this margin.
 *
 * Horizontal FOV culling is intentionally omitted — it requires tight tuning
 * to avoid popping as the camera orbits, and the behind-camera test already
 * covers the high-cost case (close orbit, bars on the far side of the formation). */
static bool bar_in_frustum(const world_obj_t *o,
                            float fx, float fy, float fz)   /* normalised 3D forward */
{
    float mid_y = FLOOR_Y + o->cur_height * 0.5f;
    float dx = o->pos_x - cam_eye.v[0];
    float dy = mid_y    - cam_eye.v[1];
    float dz = o->pos_z - cam_eye.v[2];

    float fwd_dot = dx*fx + dy*fy + dz*fz;
    float slack   = o->cur_height * CULL_HEIGHT_BIAS + BAR_WIDTH * 2.0f;
    return (fwd_dot >= -slack);
}

static void world_render(const vis_audio_t *audio) {
    /* Advance frame buffer index — must match display double-buffering */
    frame_idx = (frame_idx + 1) % FB_COUNT;

    /* Draw starfield in 2D after the screen clear, before 3D bars.
     * rdpq_sync_pipe inside starfield_draw switches from t3d fill mode back to
     * standard flat-fill for the star rectangles, then t3d_viewport_attach
     * re-enters 3D mode for the bars on top. */
    starfield_draw(s_star_bass);

    T3DVec3 up = (overhead_blend > 0.5f) ? (T3DVec3){{1, 0, 0}} : (T3DVec3){{0, 1, 0}};
    t3d_viewport_look_at(&viewport, &cam_eye, &cam_target, &up);
    t3d_viewport_attach(&viewport);

    rdpq_sync_pipe();
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    t3d_state_set_drawflags(T3D_FLAG_SHADED);

    /* Normalised 3D forward vector for behind-camera cull test. */
    float ftx = cam_target.v[0] - cam_eye.v[0];
    float fty = cam_target.v[1] - cam_eye.v[1];
    float ftz = cam_target.v[2] - cam_eye.v[2];
    float flen = sqrtf(ftx*ftx + fty*fty + ftz*ftz);
    if (flen > 0.000001f) { float inv = 1.0f / flen; ftx *= inv; fty *= inv; ftz *= inv; }

    /* Sort all bars back-to-front (painter's order) by depth along forward XZ.
     * Include inactive bars that are still shrinking (cur_height > floor + epsilon)
     * so they animate out smoothly rather than popping off. */
    int   order[NUM_BARS];
    float depths[NUM_BARS];
    int   draw_count = 0;

    for (int i = 0; i < NUM_BARS; i++) {
        /* Skip inactive bars that have fully settled to the floor */
        if (i >= obj_count && objects[i].cur_height <= BAR_MIN_HEIGHT + 0.5f) continue;
        /* Behind-camera cull — skip bars the camera cannot possibly see */
        if (!bar_in_frustum(&objects[i], ftx, fty, ftz)) continue;
        world_obj_t *o = &objects[i];
        float dx = o->pos_x - cam_eye.v[0];
        float dz = o->pos_z - cam_eye.v[2];
        float depth = dx*ftx + dz*ftz;

        /* Insertion sort: farthest first */
        int pos = draw_count++;
        while (pos > 0 && depths[pos - 1] < depth) {
            order[pos]  = order[pos - 1];
            depths[pos] = depths[pos - 1];
            pos--;
        }
        order[pos]  = i;
        depths[pos] = depth;
    }

    for (int i = 0; i < draw_count; i++)
        draw_object(&objects[order[i]]);
}

/*===========================================================================
 * Registration
 *===========================================================================*/

static const visualizer_t vis_eq_def = {
    .name    = "Visualizer EQ",
    .init    = world_init,
    .update  = world_update,
    .render  = world_render,
    .cleanup = world_cleanup,
};

VIS_REGISTER(vis_eq_def);
