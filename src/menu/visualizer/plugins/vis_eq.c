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

/* New formations: hexgrid and scatter hash constants */
#define HEXGRID_SPACING  22.0f
#define SCATTER_SEED_A   2246822519u
#define SCATTER_SEED_B   2654435761u


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
    FORM_GRID_SQUARE = 23,

    /* Geometric polygons */
    FORM_HEXAGON     = 24,
    FORM_HEPTAGON    = 25,
    FORM_OCTAGON     = 26,

    /* Mathematical curves */
    FORM_CARDIOID    = 27,
    FORM_RHODONEA    = 28,
    FORM_EPICYCLOID  = 29,
    FORM_LIMACON     = 30,
    FORM_LEMNISCATE  = 31,

    /* Multi-shape */
    FORM_CROSS       = 32,
    FORM_X_SHAPE     = 33,
    FORM_TRIPLERING  = 34,
    FORM_DOUBLESPIRAL = 35,
    FORM_HEXGRID     = 36,

    /* Screen-filling scatter / organic */
    FORM_SCATTER     = 37,
    FORM_SCATTER_WIDE = 38,
    FORM_CLOUD       = 39,
    FORM_GALAXY      = 40,
    FORM_FIBONACCI   = 41,

    /* Height-creative */
    FORM_MOUNTAIN    = 42,
    FORM_AMPHITHEATER = 43,
    FORM_FUNNEL      = 44,

    /* Organic / rhythmic */
    FORM_COMET       = 45,
    FORM_VORTEX      = 46,
    FORM_SUNBURST    = 47,
    FORM_RIPPLE      = 48,

    /* Line / curve variants */
    FORM_DIAGONAL    = 49,
    FORM_PARABOLA    = 50,
    FORM_TRIPLEWAVE  = 51,
    FORM_HEARTBEAT   = 52,

    /* Free / chaotic */
    FORM_SCATTER_RING = 53,
    FORM_CLUSTERS    = 54,
    FORM_FREE_FIELD  = 55,

    /* Multi-element */
    FORM_CHAINLINK   = 56,
    FORM_ORBIT       = 57,
    FORM_BOWTIE      = 58,
    FORM_SPIRO       = 59,

    FORM_COUNT       = 60,
} formation_t;

/* Number of framebuffers — must match display_init() call in menu.c */
#define FB_COUNT  2

typedef struct {
    int   sides;           /* polygon cross-section (3–6)           */
    float radius;          /* base XZ radius                        */
    float max_height;      /* height at band energy = 1.0           */
    float cur_height;      /* smoothed current height               */
    float pos_x, pos_z;    /* world position (lerped current)       */
    float tgt_x, tgt_z;   /* persistent morph target, updated on formation change */
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
static float   wall_time = 0.0f;   /* wall-clock time for height breathing (always advances) */
static float   cam_bar_scale = 1.0f; /* smoothed bar scale — tracks FORM_LINE ratio */
static float   overhead_blend  = 0.0f;  /* 0=orbit, 1=straight-down overhead */
static float   overhead_timer  = 0.0f;  /* accumulates; triggers overhead window */
static float   overhead_period = 55.0f; /* randomised each cycle (45–75 s) */
#define OVERHEAD_HOLD  8.0f             /* seconds to hold the overhead view */
static uint32_t form_seed = 0;          /* re-randomized on each formation transition */

/* Formation state machine */
static formation_t form_current = FORM_LINE;
static float       form_hold_remaining = 0.0f; /* seconds until we transition */
static float       form_morph_time = 0.0f;     /* elapsed time since formation changed — hold waits for morph to complete */
static float       form_cam_scale = 1.0f;      /* smoothed camera radius scale: 1=full pullback, 0.45=tight */

/* Dynamic bar count — D-up/D-down steps through 4,8,...,64 */
static int   target_bar_count  = BAR_COUNT_DEFAULT;
static float display_bar_count = BAR_COUNT_DEFAULT; /* smoothed, drives obj_count */
static bool  auto_bar_picked   = false;             /* tracks if we've picked auto target for this formation */

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
 * Band and color assignment
 *===========================================================================*/

/* Assign FFT band range and spectrum color to a bar object.
 * Color spreads bass(red) → mid(green) → treble(blue) across all active bars. */
static void assign_band_color(world_obj_t *o, int i, int n) {
    /* FFT band assignment: divide FFT_NUM_BANDS evenly across n bars */
    o->band_lo = (i * FFT_NUM_BANDS) / n;
    o->band_hi = ((i + 1) * FFT_NUM_BANDS) / n - 1;
    if (o->band_hi < o->band_lo) o->band_hi = o->band_lo;
    if (o->band_hi >= FFT_NUM_BANDS) o->band_hi = FFT_NUM_BANDS - 1;

    /* Spectrum color: bass(red) → mid(green) → treble(blue).
     * t ∈ [0,1] represents position across all active bars.
     * R: linear decay 255→0, G: sine peak (sin(t*π) is 0 at edges, 1 at t=0.5),
     * B: linear rise 0→255. Result: smooth red→yellow→green→cyan→blue gradient. */
    float t = (n > 1) ? (float)i / (n - 1) : 0.0f;
    o->color = ((uint32_t)((uint8_t)((1.0f - t) * 255)) << 24) |           /* R: 255→0 */
               ((uint32_t)((uint8_t)(fm_sinf(t * T3D_PI) * 220)) << 16) |   /* G: sin(t*π)*220 */
               ((uint32_t)((uint8_t)(t * 255)) << 8) | 0xFF;              /* B: 0→255 */
}


/*===========================================================================
 * Formation layout computation
 *===========================================================================*/

/* Compute (x, z) world position for bar i in the given formation.
 * `total` is the current active bar count — all layout math scales from it. */
/* Deterministic hash helpers for scatter formations */
static inline uint32_t bar_hash(uint32_t s, int idx) {
    uint32_t h = (s ^ (uint32_t)idx) * SCATTER_SEED_B;
    h ^= h >> 16;
    h *= SCATTER_SEED_A;
    h ^= h >> 16;
    return h;
}
static inline float hash_f(uint32_t s, int idx, int lane) {
    uint32_t h = bar_hash(s ^ (uint32_t)(lane * 1234567), idx);
    return ((float)(h >> 1) / (float)0x7FFFFFFFu) - 1.0f;
}

/* Compute (x, z) world position for bar i in the given formation.
 * `total` is the current active bar count — all layout math scales from it. */
static void formation_pos(formation_t form, int i, int total, uint32_t seed,
                          float *out_x, float *out_z, float *out_height_scale) {
    if (total < 1) total = 1;
    *out_x = 0.0f;
    *out_z = 0.0f;
    *out_height_scale = 1.0f;

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
        case FORM_GRID_SQUARE: {
            /* Square grid: cols = rows = floor(sqrt(total)) */
            int cols = (int)sqrtf((float)total);
            if (cols < 1) cols = 1;
            int col = i % cols, row = i / cols;
            float spacing = 20.0f;
            *out_x = -(cols - 1) * spacing * 0.5f + col * spacing;
            *out_z = -(cols - 1) * spacing * 0.5f + row * spacing;
            break;
        }
        case FORM_HEXAGON: {
            int bps = (total < 6) ? 1 : total / 6;
            int side = i / bps, pos = i % bps;
            float r = 95.0f;
            float a0 = side * T3D_PI / 3.0f;
            float a1 = (side + 1) * T3D_PI / 3.0f;
            float t = (bps > 1) ? (float)pos / bps : 0.5f;
            *out_x = (fm_cosf(a0) * (1.0f - t) + fm_cosf(a1) * t) * r;
            *out_z = (fm_sinf(a0) * (1.0f - t) + fm_sinf(a1) * t) * r;
            break;
        }
        case FORM_HEPTAGON: {
            int bps = (total < 7) ? 1 : total / 7;
            int side = i / bps, pos = i % bps;
            float r = 95.0f;
            float a0 = side * 2.0f * T3D_PI / 7.0f;
            float a1 = (side + 1) * 2.0f * T3D_PI / 7.0f;
            float t = (bps > 1) ? (float)pos / bps : 0.5f;
            *out_x = (fm_cosf(a0) * (1.0f - t) + fm_cosf(a1) * t) * r;
            *out_z = (fm_sinf(a0) * (1.0f - t) + fm_sinf(a1) * t) * r;
            break;
        }
        case FORM_OCTAGON: {
            int bps = (total < 8) ? 1 : total / 8;
            int side = i / bps, pos = i % bps;
            float r = 95.0f;
            float a0 = side * T3D_PI / 4.0f;
            float a1 = (side + 1) * T3D_PI / 4.0f;
            float t = (bps > 1) ? (float)pos / bps : 0.5f;
            *out_x = (fm_cosf(a0) * (1.0f - t) + fm_cosf(a1) * t) * r;
            *out_z = (fm_sinf(a0) * (1.0f - t) + fm_sinf(a1) * t) * r;
            break;
        }
        case FORM_CARDIOID: {
            float theta = (float)i / total * 2.0f * T3D_PI;
            float r = 60.0f * (1.0f + fm_cosf(theta));
            *out_x = r * fm_cosf(theta);
            *out_z = r * fm_sinf(theta);
            break;
        }
        case FORM_RHODONEA: {
            float theta = (float)i / total * 4.0f * T3D_PI;
            float r = fabsf(fm_cosf(2.5f * theta)) * 100.0f;
            *out_x = r * fm_cosf(theta);
            *out_z = r * fm_sinf(theta);
            break;
        }
        case FORM_EPICYCLOID: {
            float theta = (float)i / total * 2.0f * T3D_PI;
            float R = 40.0f, r2 = R / 3.0f;
            float k = R / r2 - 1.0f;
            *out_x = ((R - r2) * fm_cosf(theta) + r2 * fm_cosf(k * theta)) * 2.5f;
            *out_z = ((R - r2) * fm_sinf(theta) - r2 * fm_sinf(k * theta)) * 2.5f;
            break;
        }
        case FORM_LIMACON: {
            float theta = (float)i / total * 2.0f * T3D_PI;
            float r = 60.0f + 40.0f * fm_cosf(theta);
            *out_x = r * fm_cosf(theta);
            *out_z = r * fm_sinf(theta);
            break;
        }
        case FORM_LEMNISCATE: {
            int half = total / 2;
            float frac = (i < half) ? (float)i / (half > 1 ? half : 1) : (float)(i - half) / (total - half > 1 ? total - half : 1);
            float theta = (i < half)
                ? (-T3D_PI / 4.0f + frac * T3D_PI / 2.0f)
                : (3.0f * T3D_PI / 4.0f + frac * T3D_PI / 2.0f);
            float cos2t = fm_cosf(2.0f * theta);
            float r = (cos2t > 0.0f) ? sqrtf(110.0f * 110.0f * cos2t) : 0.0f;
            *out_x = r * fm_cosf(theta);
            *out_z = r * fm_sinf(theta);
            break;
        }
        case FORM_CROSS: {
            int half = total / 2;
            float arm_len = 120.0f;
            if (i < half) {
                float t = (half > 1) ? (float)i / (half - 1) : 0.5f;
                *out_x = -arm_len + t * 2.0f * arm_len;
                *out_z = 0.0f;
            } else {
                int j = i - half, n2 = total - half;
                float t = (n2 > 1) ? (float)j / (n2 - 1) : 0.5f;
                *out_x = 0.0f;
                *out_z = -arm_len + t * 2.0f * arm_len;
            }
            break;
        }
        case FORM_X_SHAPE: {
            int half = total / 2;
            float arm_len2 = 90.0f;
            float scale = arm_len2 / 1.414f;
            if (i < half) {
                float t = (half > 1) ? (float)i / (half - 1) : 0.5f;
                *out_x = -scale + t * 2.0f * scale;
                *out_z = *out_x;
            } else {
                int j = i - half, n2 = total - half;
                float t = (n2 > 1) ? (float)j / (n2 - 1) : 0.5f;
                *out_x = -scale + t * 2.0f * scale;
                *out_z = -(*out_x);
            }
            break;
        }
        case FORM_TRIPLERING: {
            int third = total / 3;
            float radii[3] = {40.0f, 75.0f, 110.0f};
            int ring = (i < third) ? 0 : (i < 2 * third) ? 1 : 2;
            int ring_start = ring * third;
            int ring_total = (ring == 2) ? (total - 2 * third) : third;
            int ring_i = i - ring_start;
            float a = (ring_total > 0) ? (float)ring_i / ring_total * 2.0f * T3D_PI : 0.0f;
            *out_x = fm_cosf(a) * radii[ring];
            *out_z = fm_sinf(a) * radii[ring];
            break;
        }
        case FORM_DOUBLESPIRAL: {
            int strand = i % 2, idx = i / 2;
            int pairs = total / 2;
            float t = (pairs > 1) ? (float)idx / (pairs - 1) : 0.5f;
            float phase = strand * T3D_PI;
            float a = t * 3.0f * T3D_PI + phase;
            float r = 20.0f + t * 90.0f;
            *out_x = fm_cosf(a) * r;
            *out_z = fm_sinf(a) * r;
            break;
        }
        case FORM_HEXGRID: {
            float s = HEXGRID_SPACING;
            float row_h = s * 0.866f;
            int cols = (int)(200.0f / s) + 1;
            int col = i % cols, row = i / cols;
            float x_off = (row % 2) ? s * 0.5f : 0.0f;
            *out_x = -cols * s * 0.5f + col * s + x_off;
            *out_z = -3.0f * row_h + row * row_h;
            break;
        }
        case FORM_SCATTER: {
            float fx = hash_f(seed, i, 0) * 130.0f;
            float fz = hash_f(seed, i, 1) * 130.0f;
            float d = sqrtf(fx * fx + fz * fz);
            if (d > 130.0f) { fx = fx / d * 130.0f; fz = fz / d * 130.0f; }
            *out_x = fx; *out_z = fz;
            break;
        }
        case FORM_SCATTER_WIDE: {
            uint32_t h = bar_hash(seed, i);
            float angle = (float)(h & 0xFFFF) / 65536.0f * 2.0f * T3D_PI;
            float r = 70.0f + (float)((h >> 16) & 0xFFFF) / 65536.0f * 80.0f;
            *out_x = fm_cosf(angle) * r;
            *out_z = fm_sinf(angle) * r;
            break;
        }
        case FORM_CLOUD: {
            float fx = (hash_f(seed, i, 0) + hash_f(seed, i, 2)) * 0.5f * 90.0f;
            float fz = (hash_f(seed, i, 1) + hash_f(seed, i, 3)) * 0.5f * 90.0f;
            *out_x = fx; *out_z = fz;
            break;
        }
        case FORM_GALAXY: {
            int arm = i % 2, idx = i / 2;
            int arm_count = total / 2;
            float t = (arm_count > 1) ? (float)idx / (arm_count - 1) : 0.5f;
            float phase = arm * T3D_PI;
            float a = t * 4.0f * T3D_PI + phase;
            float r = 5.0f + t * 105.0f + hash_f(seed ^ 0xDEAD, i, 4) * 10.0f;
            *out_x = fm_cosf(a) * r;
            *out_z = fm_sinf(a) * r;
            break;
        }
        case FORM_FIBONACCI: {
            float golden_angle = 2.399963f;
            float t = sqrtf((float)i / total);
            float r = t * 110.0f;
            float a = (float)i * golden_angle;
            *out_x = fm_cosf(a) * r;
            *out_z = fm_sinf(a) * r;
            break;
        }
        case FORM_MOUNTAIN: {
            float total_w = (total - 1) * BAR_SPACING;
            *out_x = -total_w * 0.5f + i * BAR_SPACING;
            *out_z = 0.0f;
            float t = (total > 1) ? (float)i / (total - 1) : 0.5f;
            float dist = (2.0f * t - 1.0f);
            *out_height_scale = 0.25f + 0.75f * (1.0f - dist * dist);
            break;
        }
        case FORM_AMPHITHEATER: {
            float a = (float)i / total * 2.0f * T3D_PI;
            *out_x = fm_cosf(a) * 100.0f;
            *out_z = fm_sinf(a) * 100.0f;
            float h_t = (1.0f - fm_sinf(a)) * 0.5f;
            *out_height_scale = 0.3f + h_t * 0.7f;
            break;
        }
        case FORM_FUNNEL: {
            int rings = 4;
            int bpr = total / rings;
            int ring = (rings > 1 && bpr > 0) ? (i / bpr) : 0;
            if (ring >= rings) ring = rings - 1;
            int ring_i = i - ring * bpr;
            int ring_n = (ring == rings - 1) ? (total - ring * bpr) : bpr;
            float radii_f[4] = {25.0f, 55.0f, 85.0f, 115.0f};
            float h_scales[4] = {1.4f, 1.0f, 0.65f, 0.35f};
            float r = radii_f[ring];
            float hs = h_scales[ring];
            float a = (ring_n > 0) ? (float)ring_i / ring_n * 2.0f * T3D_PI : 0.0f;
            *out_x = fm_cosf(a) * r;
            *out_z = fm_sinf(a) * r;
            *out_height_scale = hs;
            break;
        }
        case FORM_COMET: {
            int head_count = total / 3;
            if (i < head_count) {
                float a = (float)i / head_count * 2.0f * T3D_PI;
                float r = 25.0f * (float)i / head_count;
                *out_x = 80.0f + fm_cosf(a) * r;
                *out_z = fm_sinf(a) * r;
            } else {
                float t = (float)(i - head_count) / (total - head_count);
                *out_x = 80.0f - t * 210.0f;
                *out_z = hash_f(seed, i, 5) * (t * 40.0f);
            }
            break;
        }
        case FORM_VORTEX: {
            float theta = (float)(total - 1 - i) / total * 3.0f * T3D_PI;
            float r = 110.0f * expf(-0.18f * theta);
            *out_x = fm_cosf(theta) * r;
            *out_z = fm_sinf(theta) * r;
            break;
        }
        case FORM_SUNBURST: {
            int spokes = (total + 1) / 2;
            int spoke = i / 2, spoke_pos = i % 2;
            float a = (float)spoke / spokes * 2.0f * T3D_PI;
            float r = (spoke_pos == 0) ? 110.0f : 50.0f;
            *out_x = fm_cosf(a) * r;
            *out_z = fm_sinf(a) * r;
            *out_height_scale = (spoke_pos == 0) ? 0.7f : 1.2f;
            break;
        }
        case FORM_RIPPLE: {
            int rings = 4;
            int bpr = total / rings;
            int ring = (i / bpr < rings) ? i / bpr : rings - 1;
            int ring_i = i - ring * bpr;
            int ring_n = (ring == rings - 1) ? (total - ring * bpr) : bpr;
            float radii[4] = {20.0f, 42.0f, 88.0f, 115.0f};
            float h_scales[4] = {1.4f, 1.1f, 0.8f, 0.5f};
            float r = radii[ring];
            float a = (ring_n > 0) ? (float)ring_i / ring_n * 2.0f * T3D_PI : 0.0f;
            *out_x = fm_cosf(a) * r;
            *out_z = fm_sinf(a) * r;
            *out_height_scale = h_scales[ring];
            break;
        }
        case FORM_DIAGONAL: {
            float half_len = 120.0f;
            float t = (total > 1) ? (float)i / (total - 1) : 0.5f;
            *out_x = -half_len + t * 2.0f * half_len;
            *out_z = -half_len + t * 2.0f * half_len;
            break;
        }
        case FORM_PARABOLA: {
            float t = (total > 1) ? (float)i / (total - 1) : 0.5f;
            float x = -120.0f + t * 240.0f;
            *out_x = x;
            *out_z = 0.006f * x * x - 30.0f;
            break;
        }
        case FORM_TRIPLEWAVE: {
            int wave_bars = total / 3;
            int wave = (i / wave_bars < 3) ? i / wave_bars : 2;
            int wi = i - wave * wave_bars;
            int wn = (wave == 2) ? (total - 2 * wave_bars) : wave_bars;
            float t = (wn > 1) ? (float)wi / (wn - 1) : 0.5f;
            float x = -140.0f + t * 280.0f;
            float z_offset = (wave == 0) ? -45.0f : (wave == 1) ? 0.0f : 45.0f;
            float phase = wave * 2.0f * T3D_PI / 3.0f;
            *out_x = x;
            *out_z = z_offset + 30.0f * fm_sinf(t * 2.0f * T3D_PI * 2.0f + phase);
            break;
        }
        case FORM_HEARTBEAT: {
            float t = (total > 1) ? (float)i / (total - 1) : 0.5f;
            float x = -150.0f + t * 300.0f;
            float spike_z = -80.0f * expf(-x * x * 0.003f) - 30.0f * expf(-(x + 50.0f) * (x + 50.0f) * 0.02f);
            *out_x = x;
            *out_z = spike_z;
            break;
        }
        case FORM_SCATTER_RING: {
            float a = (float)i / total * 2.0f * T3D_PI;
            float base_r = 85.0f;
            float jitter = hash_f(seed, i, 6) * 30.0f;
            float r = base_r + jitter;
            *out_x = fm_cosf(a) * r;
            *out_z = fm_sinf(a) * r;
            break;
        }
        case FORM_CLUSTERS: {
            int cluster = i % 4, ci = i / 4;
            float cx[4] = {-70.0f, 70.0f, -70.0f, 70.0f};
            float cz[4] = {-70.0f, -70.0f, 70.0f, 70.0f};
            float angle_c = hash_f(seed ^ (uint32_t)cluster * 777, ci, 0) * T3D_PI;
            float r_c = hash_f(seed ^ (uint32_t)cluster * 999, ci, 1) * 0.5f * 35.0f + 5.0f;
            *out_x = cx[cluster] + fm_cosf(angle_c) * r_c;
            *out_z = cz[cluster] + fm_sinf(angle_c) * r_c;
            break;
        }
        case FORM_FREE_FIELD: {
            *out_x = hash_f(seed, i, 0) * 140.0f;
            *out_z = hash_f(seed, i, 1) * 140.0f;
            break;
        }
        case FORM_CHAINLINK: {
            int half = total / 2;
            float r_cl = 80.0f, offset = 55.0f;
            if (i < half) {
                float a = (float)i / half * 2.0f * T3D_PI;
                *out_x = -offset + fm_cosf(a) * r_cl;
                *out_z = fm_sinf(a) * r_cl;
            } else {
                float a = (float)(i - half) / (total - half) * 2.0f * T3D_PI;
                *out_x = offset + fm_cosf(a) * r_cl;
                *out_z = fm_sinf(a) * r_cl;
            }
            break;
        }
        case FORM_ORBIT: {
            int third_o = total / 3;
            int orbit = (i < third_o) ? 0 : (i < 2 * third_o) ? 1 : 2;
            int orbit_i = i - orbit * third_o;
            int orbit_n = (orbit == 2) ? (total - 2 * third_o) : third_o;
            float tilt = orbit * T3D_PI / 3.0f;
            float a_o = (orbit_n > 0) ? (float)orbit_i / orbit_n * 2.0f * T3D_PI : 0.0f;
            float ex = fm_cosf(a_o) * 110.0f;
            float ez = fm_sinf(a_o) * 50.0f;
            *out_x = ex * fm_cosf(tilt) - ez * fm_sinf(tilt);
            *out_z = ex * fm_sinf(tilt) + ez * fm_cosf(tilt);
            break;
        }
        case FORM_BOWTIE: {
            int half_bt = total / 2;
            float tri_r = 90.0f;
            if (i < half_bt) {
                float a_corners[3] = {T3D_PI, T3D_PI + 2.0f * T3D_PI / 3.0f, T3D_PI - 2.0f * T3D_PI / 3.0f};
                int bps_bt = half_bt / 3;
                int side_bt = (bps_bt > 0) ? (i / bps_bt) : 0;
                if (side_bt >= 3) side_bt = 2;
                int pos_bt = i - side_bt * bps_bt;
                float t_bt = (bps_bt > 1) ? (float)pos_bt / bps_bt : 0.5f;
                float x0 = fm_cosf(a_corners[side_bt]), z0 = fm_sinf(a_corners[side_bt]);
                float x1 = fm_cosf(a_corners[(side_bt + 1) % 3]), z1 = fm_sinf(a_corners[(side_bt + 1) % 3]);
                *out_x = (x0 * (1.0f - t_bt) + x1 * t_bt) * tri_r;
                *out_z = (z0 * (1.0f - t_bt) + z1 * t_bt) * tri_r;
            } else {
                int j = i - half_bt;
                float a_corners[3] = {0.0f, 2.0f * T3D_PI / 3.0f, -2.0f * T3D_PI / 3.0f};
                int n_bt = total - half_bt;
                int bps_bt = n_bt / 3;
                int side_bt = (bps_bt > 0) ? (j / bps_bt) : 0;
                if (side_bt >= 3) side_bt = 2;
                int pos_bt = j - side_bt * bps_bt;
                float t_bt = (bps_bt > 1) ? (float)pos_bt / bps_bt : 0.5f;
                float x0 = fm_cosf(a_corners[side_bt]), z0 = fm_sinf(a_corners[side_bt]);
                float x1 = fm_cosf(a_corners[(side_bt + 1) % 3]), z1 = fm_sinf(a_corners[(side_bt + 1) % 3]);
                *out_x = (x0 * (1.0f - t_bt) + x1 * t_bt) * tri_r;
                *out_z = (z0 * (1.0f - t_bt) + z1 * t_bt) * tri_r;
            }
            break;
        }
        case FORM_SPIRO: {
            float R_s = 70.0f, r_s = 30.0f, d_s = 60.0f;
            float theta = (float)i / total * 2.0f * T3D_PI * 3.0f;
            *out_x = (R_s - r_s) * fm_cosf(theta) + d_s * fm_cosf((R_s - r_s) / r_s * theta);
            *out_z = (R_s - r_s) * fm_sinf(theta) - d_s * fm_sinf((R_s - r_s) / r_s * theta);
            break;
        }
        default:
            *out_x = 0; *out_z = 0;
            break;
    }

    /* Validate output: clamp to world bounds and ensure finite values */
    float max_bound = 300.0f;
    if (*out_x > max_bound) *out_x = max_bound;
    if (*out_x < -max_bound) *out_x = -max_bound;
    if (*out_z > max_bound) *out_z = max_bound;
    if (*out_z < -max_bound) *out_z = -max_bound;
    /* Catch any inf/nan from exponentials or divisions */
    if (!isfinite(*out_x)) *out_x = 0.0f;
    if (!isfinite(*out_z)) *out_z = 0.0f;
}

/* Return true if a formation is screen-filling and needs wider camera pullback */
static inline bool form_is_wide(formation_t f) {
    return f == FORM_CROSS || f == FORM_X_SHAPE || f == FORM_DIAGONAL ||
           f == FORM_PARABOLA || f == FORM_TRIPLEWAVE || f == FORM_HEARTBEAT ||
           f == FORM_SCATTER_WIDE || f == FORM_FREE_FIELD || f == FORM_DOUBLESPIRAL ||
           f == FORM_CHAINLINK || f == FORM_BOWTIE || f == FORM_COMET ||
           f == FORM_DNA || f == FORM_ZIGZAG || f == FORM_WAVE || f == FORM_ARROW;
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

/* Forward declaration for update_bar_color (defined later) */
static void update_bar_color(world_obj_t *obj);

/* For LINE formation: sort bars left-to-right and reassign colors to match visual position */
static void line_sort_and_recolor(void) {
    /* Sort bar indices by pos_x (left to right) */
    int sorted_idx[NUM_BARS];
    for (int i = 0; i < obj_count; i++) sorted_idx[i] = i;
    for (int i = 1; i < obj_count; i++) {
        int k = sorted_idx[i], j = i - 1;
        while (j >= 0 && objects[sorted_idx[j]].pos_x > objects[k].pos_x)
            { sorted_idx[j + 1] = sorted_idx[j]; j--; }
        sorted_idx[j + 1] = k;
    }
    /* Reassign colors based on visual left-to-right order */
    for (int visual_pos = 0; visual_pos < obj_count; visual_pos++) {
        world_obj_t *o = &objects[sorted_idx[visual_pos]];
        uint32_t old_color = o->color;
        assign_band_color(o, visual_pos, obj_count);
        if (o->color != old_color && o->verts) update_bar_color(o);
    }
}

static void formation_set_targets(formation_t form) {
    /* Compute raw target positions in formation index order */
    float raw_x[NUM_BARS], raw_z[NUM_BARS], height_scale[NUM_BARS];
    for (int i = 0; i < obj_count; i++)
        formation_pos(form, i, obj_count, form_seed, &raw_x[i], &raw_z[i], &height_scale[i]);

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
        int bar_idx = ci[i];
        int form_idx = ti[i];
        objects[bar_idx].tgt_x = raw_x[form_idx];
        objects[bar_idx].tgt_z = raw_z[form_idx];
        /* Apply per-formation height scaling — defensive check against inf/nan */
        float scale = height_scale[form_idx];
        if (!isfinite(scale) || scale < 0.1f) scale = 1.0f;
        objects[bar_idx].max_height = BAR_MAX_HEIGHT * scale;
        if (objects[bar_idx].max_height < BAR_MIN_HEIGHT * 2)
            objects[bar_idx].max_height = BAR_MIN_HEIGHT * 2;
    }
}

/* Advance the lerp and state machine each frame */
static void formation_update(float dt) {
    /* Exponential approach (ease-out) formation morphing.
     * Formula: pos_new = pos_old + (target - pos_old) * α
     * where α = min(1, MORPH_SPEED * dt) = min(1, 0.25 * dt)
     * Frame-rate independent: always closes 25% of gap per second, regardless of fps.
     * Example at 60fps: 16.67ms × 0.25 = 4.17% per frame; at 30fps: 8.33% per frame.
     * Result: formation changes feel organic, not snappy, at any frame rate. */
    float alpha = MORPH_SPEED * dt;
    if (alpha > 1.0f) alpha = 1.0f;
    for (int i = 0; i < obj_count; i++) {
        world_obj_t *o = &objects[i];
        o->pos_x += (o->tgt_x - o->pos_x) * alpha;
        o->pos_z += (o->tgt_z - o->pos_z) * alpha;
    }

    /* Track morph time — hold timer only starts after morph completes (~15s) */
    form_morph_time += dt;
    if (form_morph_time >= 15.0f) {
        form_hold_remaining -= dt;
    }

    /* ~10s before transition, pick a new autonomous bar count target (for cinematic lead-in) */
    if (form_hold_remaining < 10.0f && !auto_bar_picked) {
        auto_bar_picked = true;
        int new_target = BAR_COUNT_MIN + (int)(rng_next() % ((BAR_COUNT_MAX - BAR_COUNT_MIN) / BAR_COUNT_STEP + 1)) * BAR_COUNT_STEP;
        /* Ensure we pick a different value if possible */
        if (new_target == target_bar_count && BAR_COUNT_MAX > BAR_COUNT_MIN) {
            new_target = (new_target == BAR_COUNT_MAX) ? (BAR_COUNT_MAX - BAR_COUNT_STEP) : (BAR_COUNT_MAX);
        }
        target_bar_count = new_target;
    }

    if (form_hold_remaining <= 0.0f) {
        formation_t next = formation_pick_next();
        form_current = next;
        form_seed = rng_next();  /* randomize seed for scatter formations */
        formation_set_targets(next);

        /* For LINE formation, sort bars by position and reassign colors left-to-right */
        if (next == FORM_LINE) {
            line_sort_and_recolor();
            form_hold_remaining = rng_float(HOLD_LINE_MIN, HOLD_LINE_MAX);
        } else {
            form_hold_remaining = rng_float(HOLD_OTHER_MIN, HOLD_OTHER_MAX);
        }

        /* Reset morph timer and auto target picker for the next formation */
        form_morph_time = 0.0f;
        auto_bar_picked = false;
    }

    /* Camera radius target: line (1.0), compact (0.45), wide screen-filling (0.65).
     * Smooth lerp so transitions are gradual. */
    float scale_target = (form_current == FORM_LINE) ? 1.0f
                       : form_is_wide(form_current) ? 0.65f
                       : 0.45f;
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
    if (!obj->verts || !obj->matrix) {
        /* Clean up any partial allocation before returning failure */
        if (obj->verts)  { free_uncached(obj->verts);  obj->verts = NULL; }
        if (obj->matrix) { free_uncached(obj->matrix); obj->matrix = NULL; }
        return false;
    }

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
    wall_time = 0.0f;
    cam_bar_scale = 1.0f;
    overhead_blend = 0.0f;
    overhead_timer = 0.0f;
    overhead_period = 45.0f + rng_float(0.0f, 30.0f);
    cam_eye    = (T3DVec3){{ fm_cosf(cam_angle) * WORLD_RADIUS,
                             CEIL_Y * 0.5f,
                             fm_sinf(cam_angle) * WORLD_RADIUS }};
    cam_target = (T3DVec3){{0, BAR_MAX_HEIGHT * 0.4f, 0}};
}

static void camera_update(const vis_audio_t *audio) {
    /* Use wall-clock frame time even when paused, so camera never stops */
    float frame_dt = (audio->dt > 0.0001f) ? audio->dt : 0.05f;  /* fallback to 50ms */
    wall_time += frame_dt;

    /* Overhead blend — driven by periodic timer and fps < 15 (hysteresis: release at 19fps) */
    float fps = (audio->dt > 0.001f) ? 1.0f / audio->dt : 20.0f;
    overhead_timer += frame_dt;
    bool perf_overhead = (overhead_blend > 0.5f) ? (fps < 19.0f) : (fps < 15.0f);
    float overhead_target = (perf_overhead ||
                             overhead_timer > overhead_period - OVERHEAD_HOLD) ? 1.0f : 0.0f;
    if (overhead_timer >= overhead_period) {
        overhead_timer  = 0.0f;
        overhead_period = 45.0f + rng_float(0.0f, 30.0f);
    }
    float blend_rate = 0.0833f;  /* Same speed in both directions — time constant ~12s (95% in ~36s) */
    overhead_blend += (overhead_target - overhead_blend) * blend_rate * frame_dt;
    if (overhead_blend < 0.0f) overhead_blend = 0.0f;
    if (overhead_blend > 1.0f) overhead_blend = 1.0f;

    /* Orbit — constant speed, always advancing */
    cam_angle += 0.10f * frame_dt;

    /* Height breathing — two incommensurate sine waves (LCG-like) ensure view never repeats.
     * Formula: h_raw = 0.72 + 0.20*sin(t*0.0785) + 0.12*sin(t*0.273)
     * Period1=80s, Period2=23s → LCM≈1840s before repeating (31 min loops).
     * Centered at 0.72 (spends ~75% overhead), then smoothstep to 0-1 range for camera blend. */
    float h_raw = 0.72f
        + 0.20f * fm_sinf(wall_time * 0.0785f)   /* ~80s period */
        + 0.12f * fm_sinf(wall_time * 0.273f);   /* ~23s period */
    if (h_raw < 0.0f) h_raw = 0.0f;
    if (h_raw > 1.0f) h_raw = 1.0f;
    float h_norm = h_raw * h_raw * (3.0f - 2.0f * h_raw); /* Smoothstep: 3t²-2t³ easing curve */

    /* Orbit camera position — smooth bar scale to avoid snap on formation transitions */
    float bar_scale_target = (form_current == FORM_LINE)
        ? (float)obj_count / (float)BAR_COUNT_DEFAULT : 1.0f;
    cam_bar_scale += (bar_scale_target - cam_bar_scale) * 0.4f * frame_dt;
    /* Orbit radius combines multiple scales:
     * - Base radius (260) scaled by bar count ratio (cam_bar_scale)
     * - Height scaling: ~35% reduction at h_norm=1 (overhead view pulls back)
     * - Formation scale: 1.0 for line (full pullback), 0.45 for compact shapes
     * - Subtle breathing: ±7% sine modulation (period ~60s) adds organic movement */
    float r = WORLD_RADIUS * cam_bar_scale
        * (1.0f - 0.35f * h_norm)           /* Height-based pullback */
        * form_cam_scale                    /* Formation-based zoom */
        * (1.0f + 0.07f * fm_sinf(wall_time * 0.11f));  /* Breathing modulation */

    float orbit_x  = fm_cosf(cam_angle) * r;
    float orbit_y  = FLOOR_Y + BAR_MAX_HEIGHT * 2.8f * h_norm;
    float orbit_z  = fm_sinf(cam_angle) * r;
    float orbit_ty = FLOOR_Y + BAR_MAX_HEIGHT * (0.5f - 0.5f * h_norm);

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

        /* Assign band range and spectrum color */
        assign_band_color(o, i, obj_count);

        float height_scale = 1.0f;
        formation_pos(FORM_LINE, i, BAR_COUNT_DEFAULT, form_seed, &o->pos_x, &o->pos_z, &height_scale);
        o->max_height = BAR_MAX_HEIGHT * height_scale;
        o->tgt_x = o->pos_x;
        o->tgt_z = o->pos_z;

        if (!build_bar(o)) {
            debugf("[VIS_EQ] WARNING: bar %d geometry allocation failed (out of memory)\n", i);
        }
    }

    /* Start in line formation, hold for a good while */
    form_current        = FORM_LINE;
    form_hold_remaining = rng_float(HOLD_LINE_MIN, HOLD_LINE_MAX);
    form_cam_scale      = 1.0f;
    auto_bar_picked     = false;
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

    /* Smoothly lerp display_bar_count toward target (~0.15 bars/sec — very smooth, ~200s to add all 60 bars) */
    float bar_diff = (float)target_bar_count - display_bar_count;
    display_bar_count += bar_diff * (audio->dt * 0.15f);
    if (display_bar_count < BAR_COUNT_MIN) display_bar_count = BAR_COUNT_MIN;
    if (display_bar_count > BAR_COUNT_MAX) display_bar_count = BAR_COUNT_MAX;
    int new_count = (int)(display_bar_count + 0.5f);

    /* When active count changes, retarget formations and reassign bands/colors
     * BUT: skip retargeting while still morphing to prevent jittery mid-morph target shifts */
    if (new_count != obj_count && form_morph_time >= 15.0f) {
        obj_count = new_count;
        formation_set_targets(form_current);

        for (int i = 0; i < obj_count; i++) {
            world_obj_t *o = &objects[i];
            uint32_t old_color = o->color;
            assign_band_color(o, i, obj_count);
            /* Update geometry only if color changed */
            if (o->color != old_color && o->verts) update_bar_color(o);
        }

        /* In LINE formation, re-sort bars by position to keep colors left-to-right */
        if (form_current == FORM_LINE) {
            line_sort_and_recolor();
        }
    }

    /* Update active bars from their assigned band range */
    float decay = expf(-22.0f * audio->dt);  /* exponential decay — smooth at all frame rates */

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

    /* Fixed star count for consistent visual density */
    star_draw_count = 48;

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

    /* Smoothly rotate up vector from (0,1,0) → (1,0,0) as overhead_blend rises.
     * Angle interpolation avoids hard switch at 0.5 blend which would cause gimbal lock:
     * up_angle = blend * π/2, then up = (sin(θ), cos(θ), 0) traces quarter circle.
     * Result always unit length and never aligned with forward vector (camera remains valid). */
    float up_angle = overhead_blend * T3D_PI * 0.5f;
    T3DVec3 up = {{ fm_sinf(up_angle), fm_cosf(up_angle), 0.0f }};
    t3d_viewport_look_at(&viewport, &cam_eye, &cam_target, &up);
    t3d_viewport_attach(&viewport);

    rdpq_sync_pipe();
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    t3d_state_set_drawflags(T3D_FLAG_SHADED);

    /* Compute normalised 3D forward vector for behind-camera cull test.
     * Dot product of bar position against this vector determines visibility:
     * if dot(pos - cam_eye, forward) < -(height * bias + bar_width*2), cull bar.
     * Prevents rendering bars that are completely behind the camera. */
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
