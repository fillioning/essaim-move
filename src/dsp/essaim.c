/**
 * Essaim — 32-voice rhythmic oscillator swarm
 * Author: fillioning
 * License: MIT
 *
 * 32 independent self-triggering voices, one per pad (notes 36–67).
 * Weighted random SVF mode per voice (50% LP, 30% BP, 20% HP).
 *
 * Pages:
 *   Global — Scale, Root, Rnd Patch, Rnd Voice, SameFreq, SameSpeed, InitFreq, Rnd Pan (+All Mono)
 *   FX     — Transpose, Fine, Saturation, Filter, Dly Mix, Dly Rate, Dly Feed, Dly Tone (+Dly Mode menu-only)
 *   Voice  — Speed, Mod, Decay, Timbre, Frequency, Noisiness, Cutoff, Volume
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define SAMPLE_RATE    44100.0f
#define SR_INV         (1.0f / 44100.0f)
#define N_VOICES       32
#define FIRST_NOTE     36
#define LAST_NOTE      (FIRST_NOTE + N_VOICES - 1)
#define TWO_PI         6.283185307f
#define DENORM         1e-20f
#define MAX_DELAY_SAMP 176400  /* 4s × 44100 */

#define SMOOTH_5MS   0.44f
#define SMOOTH_20MS  0.134f

/* ── Scale tables ──────────────────────────────────────────────────────────── */

static const int SCALE_CHROMATIC[]  = {0,1,2,3,4,5,6,7,8,9,10,11};
static const int SCALE_MAJOR[]      = {0,2,4,5,7,9,11};
static const int SCALE_MINOR[]      = {0,2,3,5,7,8,10};
static const int SCALE_PENTA[]      = {0,2,4,7,9};
static const int SCALE_WHOLE[]      = {0,2,4,6,8,10};
static const int SCALE_HARM_MIN[]   = {0,2,3,5,7,8,11};

static const int *SCALES[]     = {SCALE_CHROMATIC, SCALE_MAJOR, SCALE_MINOR, SCALE_PENTA, SCALE_WHOLE, SCALE_HARM_MIN};
static const int  SCALE_LENS[] = {12, 7, 7, 5, 6, 7};
#define N_SCALES 6

static const char *SCALE_NAMES[] = {"Chromatic","Major","Minor","Pentatonic","Whole Tone","Harm Minor"};
static const char *NOTE_NAMES[]  = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
static const char *DELAY_MODE_NAMES[] = {"Mono","Stereo","Ping-Pong"};
#define N_DELAY_MODES 3

static const int TRANSPOSE_SEMI[9] = { -36, -24, -12, -4, 0, 4, 7, 12, 24 };
static const char *TRANSPOSE_NAMES[9] = {"-3oct","-2oct","-1oct","-3rd","0","+3rd","+5th","+1oct","+2oct"};
#define N_TRANSPOSE 9

static float quantize_to_scale(float semitones, int root, int scale_idx) {
    if (scale_idx < 0 || scale_idx >= N_SCALES) scale_idx = 0;
    const int *scale = SCALES[scale_idx];
    int len = SCALE_LENS[scale_idx];
    int oct = (int)floorf(semitones / 12.0f);
    float rem = semitones - oct * 12.0f;
    float best = 999.0f;
    int best_deg = 0;
    for (int i = 0; i < len; i++) {
        float diff = fabsf(rem - scale[i]);
        float diff_wrap = fabsf(rem - (scale[i] + 12));
        if (diff_wrap < diff) {
            if (diff_wrap < best) { best = diff_wrap; best_deg = scale[i] + 12; }
        } else {
            if (diff < best) { best = diff; best_deg = scale[i]; }
        }
    }
    float note = root + oct * 12.0f + best_deg;
    return 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
}

/* ── Inline helpers ────────────────────────────────────────────────────────── */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float fast_tanh(float x) {
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* ── Biquad filter (from Octocosme) ────────────────────────────────────────── */

typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} biquad_t;

static void biquad_lpf(biquad_t *f, float freq, float q) {
    float w0 = TWO_PI * clampf(freq, 20.0f, SAMPLE_RATE * 0.49f) * SR_INV;
    float alpha = sinf(w0) / (2.0f * clampf(q, 0.1f, 20.0f));
    float cosw = cosf(w0);
    float a0_inv = 1.0f / (1.0f + alpha);
    f->b0 = (1.0f - cosw) * 0.5f * a0_inv;
    f->b1 = (1.0f - cosw) * a0_inv;
    f->b2 = f->b0;
    f->a1 = -2.0f * cosw * a0_inv;
    f->a2 = (1.0f - alpha) * a0_inv;
}

static void biquad_hpf(biquad_t *f, float freq, float q) {
    float w0 = TWO_PI * clampf(freq, 20.0f, SAMPLE_RATE * 0.49f) * SR_INV;
    float alpha = sinf(w0) / (2.0f * clampf(q, 0.1f, 20.0f));
    float cosw = cosf(w0);
    float a0_inv = 1.0f / (1.0f + alpha);
    f->b0 = (1.0f + cosw) * 0.5f * a0_inv;
    f->b1 = -(1.0f + cosw) * a0_inv;
    f->b2 = f->b0;
    f->a1 = -2.0f * cosw * a0_inv;
    f->a2 = (1.0f - alpha) * a0_inv;
}

static inline float biquad_process(biquad_t *f, float in) {
    float out = f->b0 * in + f->b1 * f->x1 + f->b2 * f->x2
              - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = in;
    f->y2 = f->y1; f->y1 = out;
    return out + DENORM;
}

/* ── Voice state ───────────────────────────────────────────────────────────── */

typedef struct {
    int   active;
    float lfo_phase, env, osc_phase;
    float svf_lp, svf_bp, svf_hp;
    int   svf_mode;
    float svf_q, pan;
    int   lfo_shape;
    float speed, mod, decay, timbre, frequency, noisiness, cutoff, volume;
    float speed_s, mod_s, decay_s, timbre_s, frequency_s, noisiness_s, cutoff_s, volume_s;
    float attack;          /* attack time in seconds (0.001–1.0) */
    int   octave;          /* octave offset: -3 to +2 (6-octave range) */
    float mod_lfo_phase;   /* slow modulation LFO for speed drift */
    float mod_lfo_rate;    /* Hz, randomized per voice at init */
    float vel_speed_mult;  /* velocity → speed multiplier (1.0–2.0) */
} voice_t;

/* ── Instance state ────────────────────────────────────────────────────────── */

typedef struct {
    voice_t voices[N_VOICES];
    int     current_voice;
    int     current_page;

    int     root_note, scale, all_mono;
    int     preset;
    int     transpose;
    float   fine;          /* -1..+1 = -1oct..+1oct continuous fine tune */
    float   fine_smooth;   /* 10ms smoothed */
    float   freq_backup[N_VOICES]; /* saved frequencies before SameFreq */
    int     freq_backup_valid;
    float   saturation, sat_smooth;
    float   filter;         /* DJ filter: 0-0.5=LP, 0.5=bypass, 0.5-1=HP */
    float   filter_smooth;

    /* Delay */
    float   dly_mix, dly_rate, dly_feedback, dly_tone;
    int     dly_mode;
    float  *dly_buf_l, *dly_buf_r;
    int     dly_write;
    float   dly_lp_l, dly_lp_r;       /* 2-pole BBD lowpass state (cascaded one-poles) */
    float   dly_lp2_l, dly_lp2_r;     /* second pole */
    float   dly_rate_smooth;           /* smoothed read position for tape warping */
    float   bbd_jitter_phase;          /* clock jitter LFO (very slow) */

    /* DJ filter (3-stage cascade, L+R) */
    biquad_t dj_lpf_l[3], dj_hpf_l[3];
    biquad_t dj_lpf_r[3], dj_hpf_r[3];

    uint32_t rng;
} essaim_t;

/* ── RNG ───────────────────────────────────────────────────────────────────── */

static inline uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x; return x;
}
static inline float rand_float(uint32_t *rng) {
    return (float)(xorshift32(rng) & 0x7FFFFFFF) / 2147483648.0f;
}
static inline float rand_range(uint32_t *rng, float lo, float hi) {
    return lo + rand_float(rng) * (hi - lo);
}

/* ── Oscillator ────────────────────────────────────────────────────────────── */

static inline float osc_shape(float phase, float timbre) {
    float sine = sinf(phase * TWO_PI);
    float tri  = 4.0f * fabsf(phase - 0.5f) - 1.0f;
    float saw  = 2.0f * phase - 1.0f;
    float sq   = phase < 0.5f ? 1.0f : -1.0f;
    if (timbre < 0.333f) { float t = timbre*3; return sine*(1-t)+tri*t; }
    else if (timbre < 0.666f) { float t = (timbre-0.333f)*3; return tri*(1-t)+saw*t; }
    else { float t = clampf((timbre-0.666f)*3, 0, 1); return saw*(1-t)+sq*t; }
}

/*
 * 6 LFO shapes (unipolar 0–1), all click-free:
 * 0 = Sine           — smooth classic
 * 1 = Triangle       — linear ramp up/down
 * 2 = Soft Saw       — raised-cosine ramp (smooth reset)
 * 3 = Soft Square    — rounded trapezoid (5% attack/release ramp)
 * 4 = Skewed Sine    — asymmetric (fast rise, slow fall)
 * 5 = Warm Pulse     — sin² bump (smooth on, smooth off)
 */
#define N_LFO_SHAPES 6

static inline float lfo_shape(float phase, int shape) {
    switch (shape) {
        case 1: /* Triangle */
            return 1.0f - 2.0f * fabsf(phase - 0.5f);
        case 2: /* Soft Saw — raised cosine (no discontinuity) */
            return 0.5f + 0.5f * cosf(phase * TWO_PI);
        case 3: { /* Soft Square — trapezoid with 5% ramps */
            float ramp = 0.05f;
            if (phase < ramp) return phase / ramp;
            if (phase < 0.5f - ramp) return 1.0f;
            if (phase < 0.5f + ramp) return 1.0f - (phase - (0.5f - ramp)) / (2.0f * ramp);
            if (phase < 1.0f - ramp) return 0.0f;
            return (phase - (1.0f - ramp)) / ramp;
        }
        case 4: /* Skewed Sine — fast rise, slow fall */
            return sinf(phase * phase * TWO_PI * 0.5f) * (phase < 0.5f ? 1.0f : cosf((phase - 0.5f) * 3.14159f));
        case 5: { /* Warm Pulse — sin² bump in first half */
            float s = sinf(phase * 3.14159f);
            return s * s;
        }
        default: /* Sine (unipolar) */
            return 0.5f + 0.5f * sinf(phase * TWO_PI);
    }
}

/* ── Randomization ─────────────────────────────────────────────────────────── */

/* Bell-curve random for mod init — average of 2 randoms, full range */
static float rand_mod_init(uint32_t *rng) {
    float a = rand_float(rng);
    float b = rand_float(rng);
    return (a + b) * 0.5f; /* bell-curve centered at 0.5, range 0..1 */
}

static void randomize_voice(essaim_t *inst, int idx) {
    voice_t *v = &inst->voices[idx];
    v->speed = rand_range(&inst->rng, 0.5f, 15.0f);
    v->mod = rand_mod_init(&inst->rng);
    v->decay = rand_range(&inst->rng, 0.02f, 1.0f);
    v->timbre = rand_float(&inst->rng);
    v->frequency = rand_float(&inst->rng);
    v->attack = rand_range(&inst->rng, 0.001f, 0.05f);
    v->octave = 0;
    v->noisiness = rand_range(&inst->rng, 0.0f, 0.3f);
    v->cutoff = rand_range(&inst->rng, 0.3f, 0.95f);
    v->volume = rand_range(&inst->rng, 0.3f, 0.9f);
    float r = rand_float(&inst->rng);
    v->svf_mode = r < 0.5f ? 0 : (r < 0.8f ? 1 : 2);
    v->svf_q = rand_range(&inst->rng, 0.3f, 0.95f);
    v->lfo_shape = (int)(rand_float(&inst->rng) * N_LFO_SHAPES) % N_LFO_SHAPES;
    v->mod_lfo_rate = rand_range(&inst->rng, 0.05f, 0.3f);
    v->mod_lfo_phase = rand_float(&inst->rng);
}

static void randomize_patch(essaim_t *inst) {
    for (int i = 0; i < N_VOICES; i++) {
        randomize_voice(inst, i);
        inst->voices[i].pan = rand_range(&inst->rng, -0.7f, 0.7f);
    }
}

static void randomize_pan(essaim_t *inst) {
    for (int i = 0; i < N_VOICES; i++)
        inst->voices[i].pan = rand_range(&inst->rng, -0.7f, 0.7f);
}

/* ── Presets ──────────────────────────────────────────────────────────────── */

#define N_PRESETS 25  /* 0=Init (random), 1-24 = named presets */

typedef struct {
    const char *name;
    /* Global */
    int scale, root_note;
    /* FX */
    int transpose; float fine, saturation, filter;
    float dly_mix, dly_rate, dly_feedback, dly_tone; int dly_mode;
    /* Voice ranges */
    float spd_lo, spd_hi, mod_lo, mod_hi, dec_lo, dec_hi;
    float tmb_lo, tmb_hi, frq_lo, frq_hi, noi_lo, noi_hi;
    float cut_lo, cut_hi, vol_lo, vol_hi;
} preset_t;

static const preset_t PRESETS[N_PRESETS] = {
    /* 0  Init (random) */ {"Init",        0, 0, 4,0,   0,   0.5, 0,0,0,0.5,2,             0,0,  0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0},
    /* 1  Swarm       */ {"Swarm",       2,11, 4,0,   0,   0.5, 0.3,0.5,0.25,0.7,1,   1,15,  0.4,0.6, 0.02,1.0, 0.0,1.0, 0.0,1.0, 0.0,0.3, 0.3,0.95, 0.3,0.9},
    /* 1  Drift       */ {"Drift",       4, 0, 4,0,   0,   0.5, 0.4,0.8,0.4,0.5,1,    0.2,2.0, 0.6,0.9, 0.3,1.5, 0.0,0.3, 0.1,0.9, 0.0,0.1, 0.5,0.9, 0.3,0.7},
    /* 2  Pulse       */ {"Pulse",       1, 0, 4,0,   0,   0.5, 0.2,0.3,0.3,0.5,1,    4,20,  0.3,0.5, 0.01,0.15,0.7,1.0, 0.2,0.8, 0.0,0.05,0.6,0.95, 0.4,0.8},
    /* 3  Fog         */ {"Fog",         2, 7, 3,0,   0,   0.35,0.5,1.2,0.5,0.3,1,    0.1,0.8, 0.4,0.6, 0.5,2.0, 0.0,0.2, 0.1,0.5, 0.0,0.15,0.2,0.5, 0.2,0.6},
    /* 4  Hive        */ {"Hive",        3, 2, 6,0,   0.1, 0.5, 0.15,0.2,0.2,0.6,1,   5,30,  0.3,0.7, 0.01,0.3, 0.3,1.0, 0.6,1.0, 0.1,0.5, 0.5,0.95, 0.3,0.7},
    /* 5  Crystal     */ {"Crystal",     1, 4, 7,0,   0,   0.5, 0.3,0.4,0.3,0.7,2,    0.5,4, 0.3,0.5, 0.2,1.0, 0.0,0.15,0.7,1.0, 0.0,0.05,0.7,0.98, 0.3,0.6},
    /* 6  Rumble      */ {"Rumble",      0, 0, 2,0,   0.3, 0.5, 0.0,0.0,0.0,0.5,0,    3,18,  0.4,0.6, 0.02,0.3, 0.5,0.9, 0.0,0.25,0.0,0.1, 0.3,0.6, 0.5,0.9},
    /* 7  Scatter     */ {"Scatter",     0, 0, 4,0,   0,   0.5, 0.25,0.15,0.35,0.6,2,  0.3,35, 0.3,0.7, 0.01,0.5, 0.0,1.0, 0.0,1.0, 0.0,0.2, 0.3,0.95, 0.2,0.8},
    /* 8  Choir       */ {"Choir",       5, 9, 4,0,   0,   0.5, 0.35,0.6,0.4,0.5,1,   0.5,3, 0.5,0.8, 0.3,1.5, 0.0,0.1, 0.3,0.7, 0.0,0.05,0.4,0.7, 0.3,0.7},
    /* 9  Glitch      */ {"Glitch",      0, 0, 4,0,   0.15,0.5, 0.2,0.1,0.3,0.8,2,    8,40,  0.3,0.7, 0.005,0.05,0.5,1.0, 0.0,1.0, 0.1,0.5, 0.4,0.95, 0.3,0.8},
    /* 10 Tide        */ {"Tide",        2, 7, 4,0,   0,   0.5, 0.5,1.0,0.45,0.4,1,   0.2,1.5, 0.4,0.6, 0.5,2.0, 0.0,0.2, 0.2,0.7, 0.0,0.1, 0.4,0.8, 0.3,0.7},
    /* 11 Spark       */ {"Spark",       3, 2, 6,0,   0.05,0.5, 0.35,0.08,0.5,0.8,2,  6,25,  0.3,0.5, 0.005,0.08,0.6,1.0, 0.4,1.0, 0.0,0.1, 0.7,0.98, 0.4,0.8},
    /* 12 Forest      */ {"Forest",      3, 4, 4,0,   0,   0.5, 0.2,0.4,0.3,0.5,1,    0.8,6, 0.4,0.6, 0.1,0.8, 0.1,0.5, 0.2,0.6, 0.05,0.2,0.4,0.8, 0.3,0.7},
    /* 13 Metal       */ {"Metal",       0, 0, 4,0,   0.4, 0.5, 0.0,0.0,0.0,0.5,0,    3,20,  0.3,0.7, 0.01,0.2, 0.6,1.0, 0.1,0.8, 0.0,0.15,0.6,0.98, 0.4,0.9},
    /* 14 Breath      */ {"Breath",      3, 9, 4,0,   0,   0.4, 0.3,0.7,0.35,0.4,1,   0.3,2, 0.4,0.6, 0.2,1.2, 0.0,0.1, 0.1,0.5, 0.4,0.9, 0.3,0.6, 0.2,0.6},
    /* 15 Clockwork   */ {"Clockwork",   1, 0, 4,0,   0,   0.5, 0.15,0.25,0.2,0.6,0,  4,4.1, 0.45,0.55,0.02,0.1, 0.3,0.7, 0.2,0.8, 0.0,0.05,0.5,0.9, 0.5,0.8},
    /* 16 Aurora      */ {"Aurora",      4, 4, 4,0,   0,   0.5, 0.45,1.5,0.4,0.5,1,   0.15,1.0,0.6,0.9,0.5,2.0, 0.0,0.15,0.2,0.9, 0.0,0.05,0.5,0.9, 0.2,0.6},
    /* 17 Dust        */ {"Dust",        0, 0, 4,0,   0,   0.5, 0.1,0.05,0.2,0.7,2,   0.5,8, 0.3,0.5, 0.005,0.03,0.3,0.8, 0.0,1.0, 0.05,0.3,0.4,0.9, 0.1,0.35},
    /* 18 Thunder     */ {"Thunder",     2, 0, 1,0,   0.5, 0.5, 0.0,0.0,0.0,0.5,0,    5,25,  0.4,0.7, 0.05,0.4, 0.6,1.0, 0.0,0.15,0.0,0.2, 0.2,0.5, 0.5,0.9},
    /* 19 Bells       */ {"Bells",       1, 4, 7,0,   0,   0.5, 0.25,0.5,0.3,0.6,1,   0.3,3, 0.3,0.5, 0.3,1.5, 0.0,0.05,0.5,1.0, 0.0,0.02,0.6,0.98, 0.3,0.7},
    /* 20 Swamp       */ {"Swamp",       2, 3, 2,0,   0.2, 0.4, 0.3,0.8,0.4,0.3,1,   0.3,3, 0.5,0.8, 0.2,1.0, 0.3,0.7, 0.0,0.3, 0.15,0.5,0.2,0.5, 0.3,0.7},
    /* 21 Firefly     */ {"Firefly",     3, 9, 6,0,   0,   0.5, 0.2,0.12,0.3,0.7,2,   3,20,  0.3,0.5, 0.005,0.06,0.2,0.6, 0.5,1.0, 0.0,0.05,0.6,0.95, 0.2,0.5},
    /* 22 Cascade     */ {"Cascade",     1, 7, 4,0,   0,   0.5, 0.5,0.3,0.6,0.6,2,    2,12,  0.4,0.6, 0.05,0.5, 0.2,0.7, 0.0,0.8, 0.0,0.1, 0.4,0.9, 0.3,0.8},
    /* 23 Zen         */ {"Zen",         3, 0, 4,0,   0,   0.5, 0.2,0.9,0.2,0.5,1,    0.3,2, 0.45,0.55,0.4,1.5, 0.0,0.1, 0.3,0.7, 0.0,0.02,0.5,0.85, 0.3,0.6},
};

static void apply_preset(essaim_t *inst, int idx) {
    if (idx < 0 || idx >= N_PRESETS) return;

    /* Init preset: fully random globals + voices */
    if (idx == 0) {
        inst->scale = (int)(rand_float(&inst->rng) * N_SCALES) % N_SCALES;
        inst->root_note = (int)(rand_float(&inst->rng) * 12) % 12;
        inst->transpose = 4; /* center */
        inst->fine = 0.0f;
        inst->saturation = rand_range(&inst->rng, 0.0f, 0.2f);
        inst->filter = 0.5f;
        inst->dly_mix = rand_range(&inst->rng, 0.0f, 0.5f);
        inst->dly_rate = rand_range(&inst->rng, 0.1f, 1.5f);
        inst->dly_feedback = rand_range(&inst->rng, 0.1f, 0.6f);
        inst->dly_tone = rand_range(&inst->rng, 0.2f, 0.8f);
        inst->dly_mode = (int)(rand_float(&inst->rng) * N_DELAY_MODES) % N_DELAY_MODES;
        randomize_patch(inst);
        inst->freq_backup_valid = 0;
        inst->preset = 0;
        return;
    }

    const preset_t *p = &PRESETS[idx];
    inst->scale = p->scale;
    inst->root_note = p->root_note;
    inst->transpose = p->transpose;
    inst->fine = p->fine;
    inst->saturation = p->saturation;
    inst->filter = p->filter;
    inst->dly_mix = p->dly_mix;
    inst->dly_rate = p->dly_rate;
    inst->dly_feedback = p->dly_feedback;
    inst->dly_tone = p->dly_tone;
    inst->dly_mode = p->dly_mode;
    for (int i = 0; i < N_VOICES; i++) {
        voice_t *v = &inst->voices[i];
        v->speed     = rand_range(&inst->rng, p->spd_lo, p->spd_hi);
        v->mod       = rand_range(&inst->rng, p->mod_lo, p->mod_hi);
        v->decay     = rand_range(&inst->rng, p->dec_lo, p->dec_hi);
        v->timbre    = rand_range(&inst->rng, p->tmb_lo, p->tmb_hi);
        v->frequency = rand_range(&inst->rng, p->frq_lo, p->frq_hi);
        v->noisiness = rand_range(&inst->rng, p->noi_lo, p->noi_hi);
        v->cutoff    = rand_range(&inst->rng, p->cut_lo, p->cut_hi);
        v->volume    = rand_range(&inst->rng, p->vol_lo, p->vol_hi);
        float r = rand_float(&inst->rng);
        v->svf_mode  = r < 0.5f ? 0 : (r < 0.8f ? 1 : 2);
        v->svf_q     = rand_range(&inst->rng, 0.3f, 0.95f);
        v->lfo_shape = (int)(rand_float(&inst->rng) * N_LFO_SHAPES) % N_LFO_SHAPES;
        v->pan       = rand_range(&inst->rng, -0.7f, 0.7f);
        v->mod_lfo_rate  = rand_range(&inst->rng, 0.05f, 0.3f);
        v->mod_lfo_phase = rand_float(&inst->rng);
    }
    inst->freq_backup_valid = 0;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    essaim_t *inst = calloc(1, sizeof(essaim_t));
    if (!inst) return NULL;

    inst->dly_buf_l = calloc(MAX_DELAY_SAMP, sizeof(float));
    inst->dly_buf_r = calloc(MAX_DELAY_SAMP, sizeof(float));
    if (!inst->dly_buf_l || !inst->dly_buf_r) {
        free(inst->dly_buf_l); free(inst->dly_buf_r); free(inst); return NULL;
    }

    inst->rng = (uint32_t)(uintptr_t)inst ^ 0xDEADBEEF;
    xorshift32(&inst->rng);

    inst->all_mono = 0;
    inst->fine_smooth = 0.0f;
    inst->sat_smooth = 0.0f;
    inst->filter_smooth = 0.5f;
    inst->dly_rate_smooth = 0.5f;
    inst->current_voice = 0; inst->current_page = 1;

    /* Init per-voice DSP state (phases, filters, envelopes) */
    for (int i = 0; i < N_VOICES; i++) {
        voice_t *v = &inst->voices[i];
        v->active = 0;
        v->lfo_phase = rand_float(&inst->rng);
        v->env = 0.0f;
        v->osc_phase = rand_float(&inst->rng);
        v->svf_lp = v->svf_bp = v->svf_hp = 0.0f;
        v->vel_speed_mult = 1.0f;
    }

    /* Apply Init preset — randomizes all params + globals */
    apply_preset(inst, 0);

    /* Snap smoothed values to match randomized targets */
    for (int i = 0; i < N_VOICES; i++) {
        voice_t *v = &inst->voices[i];
        v->speed_s=v->speed; v->mod_s=v->mod; v->decay_s=v->decay;
        v->timbre_s=v->timbre; v->frequency_s=v->frequency;
        v->noisiness_s=v->noisiness; v->cutoff_s=v->cutoff; v->volume_s=v->volume;
    }
    return inst;
}

static void destroy_instance(void *instance) {
    essaim_t *inst = (essaim_t *)instance;
    if (inst) { free(inst->dly_buf_l); free(inst->dly_buf_r); free(inst); }
}

/* ── MIDI ──────────────────────────────────────────────────────────────────── */

static void on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    essaim_t *inst = (essaim_t *)instance;
    if (len < 3) return;
    uint8_t status = msg[0] & 0xF0, note = msg[1], vel = msg[2];
    /* Accept any MIDI note and map to voices 0-31 via modulo,
       so all 32 pads work regardless of Move pad layout */
    int idx = note % N_VOICES;
    inst->current_voice = idx;
    if (status == 0x90 && vel > 0) {
        inst->voices[idx].active = 1;
        if (inst->voices[idx].env < 0.01f) inst->voices[idx].env = 0.01f;
        inst->voices[idx].vel_speed_mult = 1.0f + (float)vel / 127.0f;
    } else if (status == 0x80 || (status == 0x90 && vel == 0)) {
        inst->voices[idx].active = 0;
    } else if (status == 0xA0) {
        /* Polyphonic aftertouch — pad pressure → speed: 0=1x, 127=2x */
        int aidx = note % N_VOICES;
        inst->voices[aidx].vel_speed_mult = 1.0f + (float)vel / 127.0f;
    } else if (status == 0xD0) {
        /* Channel aftertouch — affects current voice */
        inst->voices[inst->current_voice].vel_speed_mult = 1.0f + (float)msg[1] / 127.0f;
    }
}

/* ── Parameters ────────────────────────────────────────────────────────────── */

typedef struct { const char *key; const char *label; float min, max, step; } knob_def_t;

static const knob_def_t VOICE_KNOBS[8] = {
    {"speed","Speed",0.1f,40,0.1f},{"mod","Mod",0,1,0.01f},
    {"decay","Decay",0.005f,2,0.005f},{"timbre","Timbre",0,1,0.01f},
    {"frequency","Freq",0,1,0.01f},{"noisiness","Noise",0,1,0.01f},
    {"cutoff","Cutoff",0,1,0.01f},{"volume","Volume",0,1,0.01f},
};

static const char *GLOBAL_LABELS[8] = {"Scale","Root","RndPatch","SameFreq","InitFreq","SameSpd","RndMod","RndPan"};
static const char *FX_LABELS[8] = {"Transp","Fine","Satur","Filter","Dly Mix","Dly Rate","Dly Feed","Dly Tone"};

static float *voice_param_ptr(voice_t *v, int idx) {
    switch (idx) {
        case 0: return &v->speed;    case 1: return &v->mod;
        case 2: return &v->decay;    case 3: return &v->timbre;
        case 4: return &v->frequency;case 5: return &v->noisiness;
        case 6: return &v->cutoff;   case 7: return &v->volume;
        default: return NULL;
    }
}

static void set_param(void *instance, const char *key, const char *val) {
    essaim_t *inst = (essaim_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "_level") == 0) {
        if (strcmp(val, "Global") == 0 || strcmp(val, "Essaim") == 0) inst->current_page = 0;
        else if (strcmp(val, "FX") == 0) inst->current_page = 1;
        else if (strcmp(val, "Voice") == 0) inst->current_page = 2;
        else inst->current_page = 0; /* root/unknown → Global knobs */
        return;
    }

    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_adjust")) {
        int ki = atoi(key + 5) - 1;
        float delta = atof(val);

        if (inst->current_page == 0) {
            switch (ki) {
                case 0: inst->scale = (int)clampf(inst->scale+(int)delta, 0, N_SCALES-1); break;
                case 1: inst->root_note = (int)clampf(inst->root_note+(int)delta, 0, 11); break;
                case 2: if (delta != 0) randomize_patch(inst); break;
                case 3: /* SameFreq — save backup on first use, then adjust all */
                    if (!inst->freq_backup_valid) {
                        for (int i=0;i<N_VOICES;i++) inst->freq_backup[i]=inst->voices[i].frequency;
                        inst->freq_backup_valid=1;
                    }
                    for (int i=0;i<N_VOICES;i++) inst->voices[i].frequency = clampf(inst->voices[i].frequency+delta*0.01f,0,1);
                    break;
                case 4: /* InitFreq — restore from backup */
                    if (delta != 0 && inst->freq_backup_valid) {
                        for (int i=0;i<N_VOICES;i++) inst->voices[i].frequency=inst->freq_backup[i];
                        inst->freq_backup_valid=0;
                    }
                    break;
                case 5: for (int i=0;i<N_VOICES;i++) inst->voices[i].speed = clampf(inst->voices[i].speed+delta*0.05f,0.1f,40); break;
                case 6: /* RndMod — randomize all voices' mod */
                    if (delta != 0) for (int i=0;i<N_VOICES;i++) inst->voices[i].mod = rand_mod_init(&inst->rng);
                    break;
                case 7: if (delta != 0) randomize_pan(inst); break;
            }
        } else if (inst->current_page == 1) {
            /* FX page */
            switch (ki) {
                case 0: inst->transpose = (int)clampf(inst->transpose+(int)delta, 0, N_TRANSPOSE-1); break;
                case 1: inst->fine = clampf(inst->fine+delta*0.01f, -1, 1); break;
                case 2: inst->saturation = clampf(inst->saturation+delta*0.01f, 0, 1); break;
                case 3: inst->filter = clampf(inst->filter+delta*0.01f, 0, 1); break;
                case 4: inst->dly_mix = clampf(inst->dly_mix+delta*0.01f, 0, 1); break;
                case 5: inst->dly_rate = clampf(inst->dly_rate+delta*0.001f, 0.004f, 4.0f); break;
                case 6: inst->dly_feedback = clampf(inst->dly_feedback+delta*0.01f, 0, 0.95f); break;
                case 7: inst->dly_tone = clampf(inst->dly_tone+delta*0.01f, 0, 1); break;
            }
        } else if (inst->current_page == 2) {
            /* Voice page */
            if (ki >= 0 && ki < 8) {
                voice_t *v = &inst->voices[inst->current_voice];
                float *p = voice_param_ptr(v, ki);
                if (p) *p = clampf(*p + delta * VOICE_KNOBS[ki].step,
                                   VOICE_KNOBS[ki].min, VOICE_KNOBS[ki].max);
            }
        }
        return;
    }

    /* Enum params */
    if (strcmp(key, "root_note") == 0) {
        for (int n=0;n<12;n++) if (strcmp(val,NOTE_NAMES[n])==0) { inst->root_note=n; return; }
        inst->root_note=(int)clampf(atof(val),0,11); return;
    }
    if (strcmp(key, "scale") == 0) {
        for (int n=0;n<N_SCALES;n++) if (strcmp(val,SCALE_NAMES[n])==0) { inst->scale=n; return; }
        inst->scale=(int)clampf(atof(val),0,N_SCALES-1); return;
    }
    if (strcmp(key, "dly_mode") == 0) {
        for (int n=0;n<N_DELAY_MODES;n++) if (strcmp(val,DELAY_MODE_NAMES[n])==0) { inst->dly_mode=n; return; }
        inst->dly_mode=(int)clampf(atof(val),0,N_DELAY_MODES-1); return;
    }
    if (strcmp(key, "transpose") == 0) {
        for (int n=0;n<N_TRANSPOSE;n++) if (strcmp(val,TRANSPOSE_NAMES[n])==0) { inst->transpose=n; return; }
        inst->transpose=(int)clampf(atof(val),0,N_TRANSPOSE-1); return;
    }
    if (strcmp(key, "all_mono") == 0) {
        if (strcmp(val,"On")==0) { inst->all_mono=1; for (int i=0;i<N_VOICES;i++) inst->voices[i].pan=0; return; }
        if (strcmp(val,"Off")==0) { inst->all_mono=0; return; }
        inst->all_mono = atof(val)!=0 ? 1 : 0; return;
    }

    /* Button actions */
    if (strcmp(key,"preset")==0) {
        for (int n=0;n<N_PRESETS;n++) if (strcmp(val,PRESETS[n].name)==0) { inst->preset=n; apply_preset(inst,n); return; }
        int v=(int)clampf(atof(val),0,N_PRESETS-1); inst->preset=v; apply_preset(inst,v); return;
    }
    if (strcmp(key,"rnd_patch")==0) { if (atof(val)!=0) randomize_patch(inst); return; }
    if (strcmp(key,"rnd_mod")==0)   { if (atof(val)!=0) for (int i=0;i<N_VOICES;i++) inst->voices[i].mod=rand_mod_init(&inst->rng); return; }
    if (strcmp(key,"rnd_voice")==0) { if (atof(val)!=0) randomize_voice(inst,inst->current_voice); return; }
    if (strcmp(key,"init_freq")==0) {
        if (atof(val)!=0 && inst->freq_backup_valid) {
            for (int i=0;i<N_VOICES;i++) inst->voices[i].frequency=inst->freq_backup[i];
            inst->freq_backup_valid=0;
        }
        return;
    }
    if (strcmp(key,"rnd_pan")==0)   { if (atof(val)!=0) randomize_pan(inst); return; }

    float f = atof(val);
    if (strcmp(key,"fine")==0)         { inst->fine=clampf(f,-1,1); return; }
    if (strcmp(key,"saturation")==0)   { inst->saturation=clampf(f,0,1); return; }
    if (strcmp(key,"filter")==0)       { inst->filter=clampf(f,0,1); return; }
    if (strcmp(key,"dly_mix")==0)      { inst->dly_mix=clampf(f,0,1); return; }
    if (strcmp(key,"dly_rate")==0)     { inst->dly_rate=clampf(f,0.004f,4); return; }
    if (strcmp(key,"dly_feedback")==0) { inst->dly_feedback=clampf(f,0,0.95f); return; }
    if (strcmp(key,"dly_tone")==0)     { inst->dly_tone=clampf(f,0,1); return; }
    if (strcmp(key,"same_freq")==0) {
        if (!inst->freq_backup_valid) {
            for (int i=0;i<N_VOICES;i++) inst->freq_backup[i]=inst->voices[i].frequency;
            inst->freq_backup_valid=1;
        }
        for (int i=0;i<N_VOICES;i++) inst->voices[i].frequency=clampf(f,0,1);
        return;
    }
    if (strcmp(key,"same_speed")==0)   { for (int i=0;i<N_VOICES;i++) inst->voices[i].speed=clampf(f,0.1f,40); return; }

    voice_t *v = &inst->voices[inst->current_voice];
    if (strcmp(key,"speed")==0)     { v->speed=clampf(f,0.1f,40); return; }
    if (strcmp(key,"mod")==0)       { v->mod=clampf(f,0,1); return; }
    if (strcmp(key,"decay")==0)     { v->decay=clampf(f,0.005f,2); return; }
    if (strcmp(key,"timbre")==0)    { v->timbre=clampf(f,0,1); return; }
    if (strcmp(key,"frequency")==0) { v->frequency=clampf(f,0,1); return; }
    if (strcmp(key,"noisiness")==0) { v->noisiness=clampf(f,0,1); return; }
    if (strcmp(key,"cutoff")==0)    { v->cutoff=clampf(f,0,1); return; }
    if (strcmp(key,"volume")==0)    { v->volume=clampf(f,0,1); return; }
    if (strcmp(key,"v_attack")==0)  { v->attack=clampf(f,0.001f,1.0f); return; }
    if (strcmp(key,"v_pan")==0)     { v->pan=clampf(f,-1,1); return; }
    if (strcmp(key,"v_octave")==0) {
        static const char *OCT_NAMES[6] = {"-3","-2","-1","0","+1","+2"};
        for (int n=0;n<6;n++) if (strcmp(val,OCT_NAMES[n])==0) { v->octave=n-3; return; }
        v->octave=(int)clampf(f,-3,2); return;
    }

    if (strcmp(key, "state") == 0) {
        const char *p = val;
        int rn,sc,tr,dm,am; float sat,filt,dmix,drate,dfb,dtone,fin;
        int consumed=0;
        if (sscanf(p,"%d %d %d %f %f %f %f %f %f %d %d %f%n",
                   &rn,&sc,&tr,&sat,&filt,&dmix,&drate,&dfb,&dtone,&dm,&am,&fin,&consumed)==12) {
            inst->root_note=clampf(rn,0,11); inst->scale=clampf(sc,0,N_SCALES-1);
            inst->transpose=clampf(tr,0,N_TRANSPOSE-1);
            inst->saturation=sat; inst->filter=filt;
            inst->dly_mix=dmix; inst->dly_rate=drate;
            inst->dly_feedback=dfb; inst->dly_tone=dtone;
            inst->dly_mode=dm; inst->all_mono=am; inst->fine=clampf(fin,-1,1);
            p += consumed;
        }
        for (int i=0; i<N_VOICES; i++) {
            float sp,mo,dc,tb,fq,ns,co,vo; int svf_m,lfo_s; float svf_r,pn;
            consumed=0;
            if (sscanf(p," %f %f %f %f %f %f %f %f %d %f %f %d%n",
                       &sp,&mo,&dc,&tb,&fq,&ns,&co,&vo,&svf_m,&svf_r,&pn,&lfo_s,&consumed)==12) {
                voice_t *vi=&inst->voices[i];
                vi->speed=sp; vi->mod=mo; vi->decay=dc; vi->timbre=tb;
                vi->frequency=fq; vi->noisiness=ns; vi->cutoff=co; vi->volume=vo;
                vi->svf_mode=svf_m; vi->svf_q=svf_r; vi->pan=pn; vi->lfo_shape=lfo_s;
                p += consumed;
            }
        }
    }
}

/* ── ui_hierarchy JSON ── */
static const char UI_HIERARCHY_JSON[] =
    "{\"modes\":null,\"levels\":{"
    "\"root\":{\"name\":\"Essaim\",\"knobs\":[\"scale\",\"root_note\",\"rnd_patch\",\"same_freq\",\"init_freq\",\"same_speed\",\"rnd_mod\",\"rnd_pan\"],\"params\":["
      "{\"level\":\"Global\",\"label\":\"Global\"},"
      "{\"level\":\"FX\",\"label\":\"FX\"},"
      "{\"level\":\"Voice\",\"label\":\"Voice\"}"
    "]},"
    "\"Global\":{\"name\":\"Global\","
      "\"knobs\":[\"scale\",\"root_note\",\"rnd_patch\",\"same_freq\",\"init_freq\",\"same_speed\",\"rnd_mod\",\"rnd_pan\"],"
      "\"params\":[\"scale\",\"root_note\",\"rnd_patch\",\"same_freq\",\"init_freq\",\"same_speed\",\"rnd_mod\",\"rnd_pan\",\"all_mono\",\"rnd_voice\",\"preset\"]"
    "},"
    "\"FX\":{\"name\":\"FX\","
      "\"knobs\":[\"transpose\",\"fine\",\"saturation\",\"filter\",\"dly_mix\",\"dly_rate\",\"dly_feedback\",\"dly_tone\"],"
      "\"params\":[\"transpose\",\"fine\",\"saturation\",\"filter\",\"dly_mix\",\"dly_rate\",\"dly_feedback\",\"dly_tone\",\"dly_mode\"]"
    "},"
    "\"Voice\":{\"name\":\"Voice\","
      "\"knobs\":[\"speed\",\"mod\",\"decay\",\"timbre\",\"frequency\",\"noisiness\",\"cutoff\",\"volume\"],"
      "\"params\":[\"speed\",\"mod\",\"decay\",\"timbre\",\"frequency\",\"noisiness\",\"cutoff\",\"volume\",\"v_attack\",\"v_pan\",\"v_octave\"]"
    "}"
    "}}";

static const char CHAIN_PARAMS_JSON[] =
    "["
    "{\"key\":\"scale\",\"name\":\"Scale\",\"type\":\"enum\","
      "\"options\":[\"Chromatic\",\"Major\",\"Minor\",\"Pentatonic\",\"Whole Tone\",\"Harm Minor\"]},"
    "{\"key\":\"root_note\",\"name\":\"Root Note\",\"type\":\"enum\","
      "\"options\":[\"C\",\"C#\",\"D\",\"D#\",\"E\",\"F\",\"F#\",\"G\",\"G#\",\"A\",\"A#\",\"B\"]},"
    "{\"key\":\"rnd_patch\",\"name\":\"Rnd Patch\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":1},"
    "{\"key\":\"same_freq\",\"name\":\"Same Freq\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"init_freq\",\"name\":\"Init Freq\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":1},"
    "{\"key\":\"same_speed\",\"name\":\"Same Speed\",\"type\":\"float\",\"min\":0.1,\"max\":40,\"step\":0.1},"
    "{\"key\":\"rnd_mod\",\"name\":\"Rnd Mod\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":1},"
    "{\"key\":\"rnd_pan\",\"name\":\"Rnd Pan\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":1},"
    "{\"key\":\"all_mono\",\"name\":\"All Mono\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
    "{\"key\":\"rnd_voice\",\"name\":\"Rnd Voice\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":1},"
    "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"enum\","
      "\"options\":[\"Init\",\"Swarm\",\"Drift\",\"Pulse\",\"Fog\",\"Hive\",\"Crystal\",\"Rumble\",\"Scatter\","
      "\"Choir\",\"Glitch\",\"Tide\",\"Spark\",\"Forest\",\"Metal\",\"Breath\",\"Clockwork\","
      "\"Aurora\",\"Dust\",\"Thunder\",\"Bells\",\"Swamp\",\"Firefly\",\"Cascade\",\"Zen\"]},"
    "{\"key\":\"transpose\",\"name\":\"Transpose\",\"type\":\"enum\","
      "\"options\":[\"-3oct\",\"-2oct\",\"-1oct\",\"-3rd\",\"0\",\"+3rd\",\"+5th\",\"+1oct\",\"+2oct\"]},"
    "{\"key\":\"fine\",\"name\":\"Fine\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01},"
    "{\"key\":\"saturation\",\"name\":\"Saturation\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"filter\",\"name\":\"Filter\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"dly_mix\",\"name\":\"Dly Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"dly_rate\",\"name\":\"Dly Rate\",\"type\":\"float\",\"min\":0.004,\"max\":4.0,\"step\":0.001},"
    "{\"key\":\"dly_feedback\",\"name\":\"Dly Feed\",\"type\":\"float\",\"min\":0,\"max\":0.95,\"step\":0.01},"
    "{\"key\":\"dly_tone\",\"name\":\"Dly Tone\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"dly_mode\",\"name\":\"Dly Mode\",\"type\":\"enum\","
      "\"options\":[\"Mono\",\"Stereo\",\"Ping-Pong\"]},"
    "{\"key\":\"speed\",\"name\":\"Speed\",\"type\":\"float\",\"min\":0.1,\"max\":40,\"step\":0.1},"
    "{\"key\":\"mod\",\"name\":\"Mod\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"decay\",\"name\":\"Decay\",\"type\":\"float\",\"min\":0.005,\"max\":2.0,\"step\":0.005},"
    "{\"key\":\"timbre\",\"name\":\"Timbre\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"frequency\",\"name\":\"Freq\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"noisiness\",\"name\":\"Noise\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"cutoff\",\"name\":\"Cutoff\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"volume\",\"name\":\"Volume\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_attack\",\"name\":\"Attack\",\"type\":\"float\",\"min\":0.001,\"max\":1.0,\"step\":0.001},"
    "{\"key\":\"v_pan\",\"name\":\"Pan\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_octave\",\"name\":\"Octave\",\"type\":\"enum\","
      "\"options\":[\"-3\",\"-2\",\"-1\",\"0\",\"+1\",\"+2\"]}"
    "]";

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    essaim_t *inst = (essaim_t *)instance;

    if (strcmp(key,"ui_hierarchy")==0) {
        int len=(int)strlen(UI_HIERARCHY_JSON);
        if (len>=buf_len) len=buf_len-1;
        memcpy(buf,UI_HIERARCHY_JSON,len); buf[len]='\0'; return len;
    }
    if (strcmp(key,"chain_params")==0) {
        int len=(int)strlen(CHAIN_PARAMS_JSON);
        if (len>=buf_len) len=buf_len-1;
        memcpy(buf,CHAIN_PARAMS_JSON,len); buf[len]='\0'; return len;
    }
    if (strcmp(key,"name")==0) return snprintf(buf,buf_len,"Essaim");

    /* Knob overlay */
    if (strncmp(key,"knob_",5)==0 && strstr(key,"_name")) {
        int idx=atoi(key+5)-1;
        if (idx<0||idx>7) return snprintf(buf,buf_len," ");
        if (inst->current_page==0) return snprintf(buf,buf_len,"%s",GLOBAL_LABELS[idx]);
        else if (inst->current_page==1) return snprintf(buf,buf_len,"%s",FX_LABELS[idx]);
        else return snprintf(buf,buf_len,"V%d %s",inst->current_voice+1,VOICE_KNOBS[idx].label);
    }

    if (strncmp(key,"knob_",5)==0 && strstr(key,"_value")) {
        int idx=atoi(key+5)-1;
        if (idx<0||idx>7) return snprintf(buf,buf_len," ");

        if (inst->current_page==0) {
            switch (idx) {
                case 0: return snprintf(buf,buf_len,"%s",SCALE_NAMES[inst->scale]);
                case 1: return snprintf(buf,buf_len,"%s",NOTE_NAMES[inst->root_note]);
                case 2: case 4: case 6: case 7: return snprintf(buf,buf_len,"Turn");
                case 3: return snprintf(buf,buf_len,"%d%%",(int)(inst->voices[inst->current_voice].frequency*100));
                case 5: return snprintf(buf,buf_len,"%.1fHz",inst->voices[inst->current_voice].speed);
            }
        } else if (inst->current_page==1) {
            /* FX page: Transp, Fine, Satur, Filter, Dly Mix, Dly Rate, Dly Feed, Dly Tone */
            switch (idx) {
                case 0: return snprintf(buf,buf_len,"%s",TRANSPOSE_NAMES[inst->transpose]);
                case 1: {
                    float semi = inst->fine * 12.0f;
                    if (fabsf(semi) < 0.05f) return snprintf(buf,buf_len,"0 st");
                    return snprintf(buf,buf_len,"%+.1f st",semi);
                }
                case 2: return snprintf(buf,buf_len,"%d%%",(int)(inst->saturation*100));
                case 3: {
                    float f=inst->filter;
                    if (f<0.49f) return snprintf(buf,buf_len,"LP %d%%",(int)((0.49f-f)/0.49f*100));
                    if (f>0.51f) return snprintf(buf,buf_len,"HP %d%%",(int)((f-0.51f)/0.49f*100));
                    return snprintf(buf,buf_len,"Off");
                }
                case 4: return snprintf(buf,buf_len,"%d%%",(int)(inst->dly_mix*100));
                case 5:
                    if (inst->dly_rate<1) return snprintf(buf,buf_len,"%dms",(int)(inst->dly_rate*1000));
                    return snprintf(buf,buf_len,"%.2fs",inst->dly_rate);
                case 6: return snprintf(buf,buf_len,"%d%%",(int)(inst->dly_feedback*100));
                case 7: return snprintf(buf,buf_len,"%d%%",(int)(inst->dly_tone*100));
            }
        } else {
            /* Voice page */
            voice_t *v=&inst->voices[inst->current_voice];
            float *p=voice_param_ptr(v,idx);
            if (p) {
                if (idx==0) return snprintf(buf,buf_len,"%.1fHz",*p);
                if (idx==1) return snprintf(buf,buf_len,"%+d%%",(int)((*p-0.5f)*200));
                if (idx==2) return snprintf(buf,buf_len,"%dms",(int)(*p*1000));
                return snprintf(buf,buf_len,"%d%%",(int)(*p*100));
            }
        }
        return snprintf(buf,buf_len," ");
    }

    /* Direct param gets */
    if (strcmp(key,"scale")==0) return snprintf(buf,buf_len,"%s",SCALE_NAMES[inst->scale]);
    if (strcmp(key,"root_note")==0) return snprintf(buf,buf_len,"%s",NOTE_NAMES[inst->root_note]);
    if (strcmp(key,"all_mono")==0) return snprintf(buf,buf_len,"%s",inst->all_mono?"On":"Off");
    if (strcmp(key,"preset")==0) return snprintf(buf,buf_len,"%s",PRESETS[inst->preset].name);
    if (strcmp(key,"rnd_patch")==0||strcmp(key,"rnd_mod")==0||strcmp(key,"rnd_voice")==0||strcmp(key,"init_freq")==0||strcmp(key,"rnd_pan")==0) return snprintf(buf,buf_len,"0");
    if (strcmp(key,"same_freq")==0) return snprintf(buf,buf_len,"%.4f",inst->voices[inst->current_voice].frequency);
    if (strcmp(key,"same_speed")==0) return snprintf(buf,buf_len,"%.4f",inst->voices[inst->current_voice].speed);
    if (strcmp(key,"transpose")==0) return snprintf(buf,buf_len,"%s",TRANSPOSE_NAMES[inst->transpose]);
    if (strcmp(key,"fine")==0) return snprintf(buf,buf_len,"%.4f",inst->fine);
    if (strcmp(key,"saturation")==0) return snprintf(buf,buf_len,"%.4f",inst->saturation);
    if (strcmp(key,"filter")==0) return snprintf(buf,buf_len,"%.4f",inst->filter);
    if (strcmp(key,"dly_mix")==0) return snprintf(buf,buf_len,"%.4f",inst->dly_mix);
    if (strcmp(key,"dly_rate")==0) return snprintf(buf,buf_len,"%.4f",inst->dly_rate);
    if (strcmp(key,"dly_feedback")==0) return snprintf(buf,buf_len,"%.4f",inst->dly_feedback);
    if (strcmp(key,"dly_tone")==0) return snprintf(buf,buf_len,"%.4f",inst->dly_tone);
    if (strcmp(key,"dly_mode")==0) return snprintf(buf,buf_len,"%s",DELAY_MODE_NAMES[inst->dly_mode]);

    voice_t *v=&inst->voices[inst->current_voice];
    if (strcmp(key,"speed")==0) return snprintf(buf,buf_len,"%.4f",v->speed);
    if (strcmp(key,"mod")==0) return snprintf(buf,buf_len,"%.4f",v->mod);
    if (strcmp(key,"decay")==0) return snprintf(buf,buf_len,"%.4f",v->decay);
    if (strcmp(key,"timbre")==0) return snprintf(buf,buf_len,"%.4f",v->timbre);
    if (strcmp(key,"frequency")==0) return snprintf(buf,buf_len,"%.4f",v->frequency);
    if (strcmp(key,"noisiness")==0) return snprintf(buf,buf_len,"%.4f",v->noisiness);
    if (strcmp(key,"cutoff")==0) return snprintf(buf,buf_len,"%.4f",v->cutoff);
    if (strcmp(key,"volume")==0) return snprintf(buf,buf_len,"%.4f",v->volume);
    if (strcmp(key,"v_attack")==0) return snprintf(buf,buf_len,"%.4f",v->attack);
    if (strcmp(key,"v_pan")==0) return snprintf(buf,buf_len,"%.4f",v->pan);
    if (strcmp(key,"v_octave")==0) {
        static const char *OCT_NAMES[6] = {"-3","-2","-1","0","+1","+2"};
        return snprintf(buf,buf_len,"%s",OCT_NAMES[v->octave+3]);
    }

    if (strcmp(key,"state")==0) {
        int pos=snprintf(buf,buf_len,"%d %d %d %f %f %f %f %f %f %d %d %f",
            inst->root_note,inst->scale,inst->transpose,
            inst->saturation,inst->filter,inst->dly_mix,inst->dly_rate,
            inst->dly_feedback,inst->dly_tone,inst->dly_mode,inst->all_mono,inst->fine);
        for (int i=0;i<N_VOICES&&pos<buf_len-1;i++) {
            voice_t *vi=&inst->voices[i];
            pos+=snprintf(buf+pos,buf_len-pos," %f %f %f %f %f %f %f %f %d %f %f %d",
                vi->speed,vi->mod,vi->decay,vi->timbre,
                vi->frequency,vi->noisiness,vi->cutoff,vi->volume,
                vi->svf_mode,vi->svf_q,vi->pan,vi->lfo_shape);
        }
        return pos;
    }
    return -1;
}

/* ── Audio processing ──────────────────────────────────────────────────────── */

static void render_block(void *instance, int16_t *out_lr, int frames) {
    essaim_t *inst = (essaim_t *)instance;

    /* Fine tune: ~10ms block-rate smoothing, ±1 = ±12 semitones (1 octave) */
    inst->fine_smooth += 0.25f * (inst->fine - inst->fine_smooth);
    float transpose_ratio = powf(2.0f, (TRANSPOSE_SEMI[inst->transpose] + inst->fine_smooth * 12.0f) / 12.0f);

    /* ── Per-voice pre-compute with smoothing ── */
    struct {
        float lfo_inc, base_freq, f_coeff, q_fb, nm, pan_l, pan_r, decay_coeff, attack_coeff;
        int alive;
    } vc[N_VOICES];

    for (int vi = 0; vi < N_VOICES; vi++) {
        voice_t *v = &inst->voices[vi];
        vc[vi].alive = (v->active || v->env >= 0.001f);
        if (!vc[vi].alive) continue;

        v->speed_s     += SMOOTH_5MS  * (v->speed     - v->speed_s);
        v->mod_s       += SMOOTH_5MS  * (v->mod       - v->mod_s);
        v->decay_s     += SMOOTH_5MS  * (v->decay     - v->decay_s);
        v->timbre_s    += SMOOTH_5MS  * (v->timbre    - v->timbre_s);
        v->frequency_s += SMOOTH_5MS  * (v->frequency - v->frequency_s);
        v->noisiness_s += SMOOTH_5MS  * (v->noisiness - v->noisiness_s);
        v->cutoff_s    += SMOOTH_20MS * (v->cutoff    - v->cutoff_s);
        v->volume_s    += SMOOTH_5MS  * (v->volume    - v->volume_s);

        vc[vi].lfo_inc = v->speed_s * v->vel_speed_mult * SR_INV;
        float semitones = v->frequency_s * 84.0f;
        float oct_ratio = powf(2.0f, (float)v->octave);
        vc[vi].base_freq = quantize_to_scale(semitones, inst->root_note + 12, inst->scale) * transpose_ratio * oct_ratio;
        float cutoff_shaped = v->cutoff_s * v->cutoff_s;
        float fc = 30.0f * powf(433.0f, cutoff_shaped);
        vc[vi].f_coeff = 2.0f * sinf(3.14159f * fc / SAMPLE_RATE);
        if (vc[vi].f_coeff > 1.6f) vc[vi].f_coeff = 1.6f;
        vc[vi].q_fb = 1.0f - v->svf_q;
        vc[vi].nm = v->noisiness_s * 0.5f;
        float pan_val = inst->all_mono ? 0.0f : v->pan;
        float pan01 = (pan_val + 1.0f) * 0.5f;
        vc[vi].pan_l = cosf(pan01 * 1.5708f);
        vc[vi].pan_r = sinf(pan01 * 1.5708f);
        vc[vi].decay_coeff = 1.0f - 1.0f / (v->decay_s * SAMPLE_RATE + 1.0f);
        vc[vi].attack_coeff = 1.0f / (v->attack * SAMPLE_RATE + 1.0f);
    }

    /* ── Saturation: smooth drive, gentler curve ── */
    inst->sat_smooth += SMOOTH_20MS * (inst->saturation - inst->sat_smooth);
    float sat = inst->sat_smooth;
    /* Quadratic drive curve: 1 at 0%, ~3 at 50%, 8 at 100% — gentle warmth */
    float drive = 1.0f + sat * sat * 7.0f;
    /* Compensation: normalize at typical RMS level */
    float sat_comp = 0.5f / (fast_tanh(0.5f * drive) + 0.001f);

    /* ── DJ filter: update biquad coefficients ── */
    inst->filter_smooth += 0.05f * (inst->filter - inst->filter_smooth);
    float fs = inst->filter_smooth;
    if (fs < 0.49f) {
        float t = (0.49f - fs) / 0.49f;
        float lp_f = 18000.0f * powf(200.0f / 18000.0f, t);
        for (int s = 0; s < 3; s++) {
            biquad_lpf(&inst->dj_lpf_l[s], lp_f, 0.707f);
            biquad_lpf(&inst->dj_lpf_r[s], lp_f, 0.707f);
        }
    } else if (fs > 0.51f) {
        float t = (fs - 0.51f) / 0.49f;
        float hp_f = 20.0f * powf(400.0f, t);
        for (int s = 0; s < 3; s++) {
            biquad_hpf(&inst->dj_hpf_l[s], hp_f, 0.707f);
            biquad_hpf(&inst->dj_hpf_r[s], hp_f, 0.707f);
        }
    }

    /* ── Delay: rate smoothing (~20ms settle — brief tape glide on knob turn, then stable) ── */
    inst->dly_rate_smooth += 0.002f * (inst->dly_rate - inst->dly_rate_smooth);
    float dly_rate_s = inst->dly_rate_smooth;
    float dly_tone = inst->dly_tone;
    float dly_fb = inst->dly_feedback;
    float dly_mix = inst->dly_mix;

    /* ── Per-sample loop ── */
    for (int i = 0; i < frames; i++) {
        float mix_l = 0.0f, mix_r = 0.0f;

        for (int vi = 0; vi < N_VOICES; vi++) {
            if (!vc[vi].alive) continue;
            voice_t *v = &inst->voices[vi];

            /* Slow mod LFO drifts the speed (like someone turning the knob) */
            v->mod_lfo_phase += v->mod_lfo_rate * SR_INV;
            if (v->mod_lfo_phase >= 1.0f) v->mod_lfo_phase -= 1.0f;
            float mod_lfo_val = sinf(v->mod_lfo_phase * TWO_PI); /* -1..+1 smooth */
            float mod_amount = (v->mod_s - 0.5f) * 2.0f; /* -1..+1 from knob */
            float speed_mult = 1.0f + mod_amount * mod_lfo_val;
            if (speed_mult < 0.05f) speed_mult = 0.05f;

            /* Main rhythmic LFO at modulated speed */
            v->lfo_phase += vc[vi].lfo_inc * speed_mult;
            if (v->lfo_phase >= 1.0f) v->lfo_phase -= 1.0f;
            float lfo_val = lfo_shape(v->lfo_phase, v->lfo_shape);

            if (v->active) {
                v->env += (1.05f - v->env) * vc[vi].attack_coeff;
            } else {
                v->env *= vc[vi].decay_coeff;
                if (v->env < 0.0001f) { v->env = 0; vc[vi].alive = 0; continue; }
            }

            v->osc_phase += vc[vi].base_freq * SR_INV;
            if (v->osc_phase >= 1.0f) v->osc_phase -= 1.0f;
            float osc = osc_shape(v->osc_phase, v->timbre_s);
            float noise = (float)(int32_t)xorshift32(&inst->rng) / 2147483648.0f;
            float sig = osc * (1.0f - vc[vi].nm) + noise * vc[vi].nm;

            v->svf_hp = sig - v->svf_lp - vc[vi].q_fb * v->svf_bp;
            v->svf_bp += vc[vi].f_coeff * v->svf_hp;
            v->svf_lp += vc[vi].f_coeff * v->svf_bp;
            v->svf_bp = clampf(v->svf_bp, -4, 4);
            v->svf_lp = clampf(v->svf_lp, -4, 4);

            float filtered;
            switch (v->svf_mode) {
                case 1: filtered = v->svf_bp * vc[vi].q_fb; break;
                case 2: filtered = v->svf_hp; break;
                default: filtered = v->svf_lp; break;
            }
            float out = filtered * lfo_val * v->env * v->volume_s;
            mix_l += out * vc[vi].pan_l;
            mix_r += out * vc[vi].pan_r;
        }

        mix_l *= 0.15f;
        mix_r *= 0.15f;

        /* DJ filter (3-stage cascade, before saturation) */
        if (fs < 0.49f) {
            for (int s = 0; s < 3; s++) mix_l = biquad_process(&inst->dj_lpf_l[s], mix_l);
            for (int s = 0; s < 3; s++) mix_r = biquad_process(&inst->dj_lpf_r[s], mix_r);
        } else if (fs > 0.51f) {
            for (int s = 0; s < 3; s++) mix_l = biquad_process(&inst->dj_hpf_l[s], mix_l);
            for (int s = 0; s < 3; s++) mix_r = biquad_process(&inst->dj_hpf_r[s], mix_r);
        }

        /* Saturation */
        if (sat > 0.005f) {
            mix_l = fast_tanh(mix_l * drive) * sat_comp;
            mix_r = fast_tanh(mix_r * drive) * sat_comp;
        }

        /* BBD-style delay with 2-pole filter, soft saturation, clock jitter */
        if (dly_mix > 0.001f) {
            /* Smoothed fractional read position for tape-style pitch warping */
            float delay_f = dly_rate_s * SAMPLE_RATE;
            float read_f = (float)inst->dly_write - delay_f;
            if (read_f < 0) read_f += MAX_DELAY_SAMP;
            int rp0 = (int)read_f;
            float frac = read_f - rp0;
            int rp1 = rp0 + 1;
            if (rp0 >= MAX_DELAY_SAMP) rp0 -= MAX_DELAY_SAMP;
            if (rp1 >= MAX_DELAY_SAMP) rp1 -= MAX_DELAY_SAMP;

            /* Linear interpolation for smooth pitch glide */
            float tap_l = inst->dly_buf_l[rp0] * (1-frac) + inst->dly_buf_l[rp1] * frac;
            float tap_r = inst->dly_buf_r[rp0] * (1-frac) + inst->dly_buf_r[rp1] * frac;

            /* BBD clock jitter: slow LFO modulates the filter cutoff (~0.15 Hz) */
            inst->bbd_jitter_phase += 0.15f * SR_INV;
            if (inst->bbd_jitter_phase >= 1.0f) inst->bbd_jitter_phase -= 1.0f;
            float jitter = sinf(inst->bbd_jitter_phase * TWO_PI) * 0.04f; /* 4% depth */

            /* 2-pole cascaded one-pole lowpass in feedback (BBD anti-aliasing character) */
            float fb_l, fb_r;
            float tape_noise = 0.0f;
            float sig_level = fabsf(tap_l) + fabsf(tap_r);

            if (dly_tone < 0.49f) {
                /* Dark: LP cutoff 800 Hz → 6 kHz, modulated by jitter */
                float base_c = 0.08f + (dly_tone / 0.49f) * 0.72f;
                float lp_c = clampf(base_c + jitter, 0.04f, 0.92f);
                /* Pole 1 */
                inst->dly_lp_l  += lp_c * (tap_l - inst->dly_lp_l);
                inst->dly_lp_r  += lp_c * (tap_r - inst->dly_lp_r);
                /* Pole 2 — steeper rolloff, warmer character */
                inst->dly_lp2_l += lp_c * (inst->dly_lp_l - inst->dly_lp2_l);
                inst->dly_lp2_r += lp_c * (inst->dly_lp_r - inst->dly_lp2_r);
                fb_l = inst->dly_lp2_l;
                fb_r = inst->dly_lp2_r;
                /* Subtle tape hiss — only when signal present */
                if (sig_level > 0.001f) {
                    float hiss_depth = (0.49f - dly_tone) / 0.49f;
                    tape_noise = ((float)(int32_t)xorshift32(&inst->rng) / 2147483648.0f)
                               * 0.0004f * hiss_depth;
                }
            } else if (dly_tone > 0.51f) {
                /* Bright: still apply gentle BBD rolloff (~8 kHz) with jitter */
                float lp_c = clampf(0.85f + jitter, 0.7f, 0.95f);
                inst->dly_lp_l  += lp_c * (tap_l - inst->dly_lp_l);
                inst->dly_lp_r  += lp_c * (tap_r - inst->dly_lp_r);
                inst->dly_lp2_l += lp_c * (inst->dly_lp_l - inst->dly_lp2_l);
                inst->dly_lp2_r += lp_c * (inst->dly_lp_r - inst->dly_lp2_r);
                fb_l = inst->dly_lp2_l;
                fb_r = inst->dly_lp2_r;
                /* Vinyl crackle — only when signal present */
                if (sig_level > 0.001f) {
                    float crackle_depth = (dly_tone - 0.51f) / 0.49f;
                    if (rand_float(&inst->rng) < crackle_depth * 0.001f)
                        tape_noise = rand_range(&inst->rng, -0.004f, 0.004f);
                }
            } else {
                /* Neutral: gentle BBD rolloff only, jitter-modulated */
                float lp_c = clampf(0.88f + jitter, 0.75f, 0.95f);
                inst->dly_lp_l  += lp_c * (tap_l - inst->dly_lp_l);
                inst->dly_lp_r  += lp_c * (tap_r - inst->dly_lp_r);
                inst->dly_lp2_l += lp_c * (inst->dly_lp_l - inst->dly_lp2_l);
                inst->dly_lp2_r += lp_c * (inst->dly_lp_r - inst->dly_lp2_r);
                fb_l = inst->dly_lp2_l;
                fb_r = inst->dly_lp2_r;
            }

            /* BBD soft saturation in feedback — gentle, no overdrive */
            /* x / (1 + |x|) curve: unity gain at low levels, soft limit at ±1 */
            fb_l = fb_l * dly_fb + tape_noise;
            fb_r = fb_r * dly_fb + tape_noise;
            fb_l = fb_l / (1.0f + fabsf(fb_l * 0.8f));
            fb_r = fb_r / (1.0f + fabsf(fb_r * 0.8f));

            switch (inst->dly_mode) {
                case 0:
                    inst->dly_buf_l[inst->dly_write] = (mix_l+mix_r)*0.5f + fb_l;
                    inst->dly_buf_r[inst->dly_write] = inst->dly_buf_l[inst->dly_write];
                    break;
                case 1:
                    inst->dly_buf_l[inst->dly_write] = mix_l + fb_l;
                    inst->dly_buf_r[inst->dly_write] = mix_r + fb_r;
                    break;
                case 2:
                    inst->dly_buf_l[inst->dly_write] = mix_r + fb_r;
                    inst->dly_buf_r[inst->dly_write] = mix_l + fb_l;
                    break;
            }

            /* Wet mix: gentle gain, soft-clipped */
            mix_l += fast_tanh(tap_l * 1.2f) * dly_mix;
            mix_r += fast_tanh(tap_r * 1.2f) * dly_mix;

            inst->dly_write++;
            if (inst->dly_write >= MAX_DELAY_SAMP) inst->dly_write = 0;
        }

        /* Final soft clip */
        mix_l = fast_tanh(mix_l) * 0.95f;
        mix_r = fast_tanh(mix_r) * 0.95f;
        out_lr[i*2]   = (int16_t)(clampf(mix_l,-1,1) * 32767.0f);
        out_lr[i*2+1] = (int16_t)(clampf(mix_r,-1,1) * 32767.0f);
    }
}

/* ── API v2 export ─────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t api_version;
    void* (*create_instance)(const char *, const char *);
    void  (*destroy_instance)(void *);
    void  (*on_midi)(void *, const uint8_t *, int, int);
    void  (*set_param)(void *, const char *, const char *);
    int   (*get_param)(void *, const char *, char *, int);
    int   (*get_error)(void *, char *, int);
    void  (*render_block)(void *, int16_t *, int);
} plugin_api_v2_t;

__attribute__((visibility("default")))
plugin_api_v2_t* move_plugin_init_v2(const void *host) {
    (void)host;
    static plugin_api_v2_t api = {
        .api_version      = 2,
        .create_instance  = create_instance,
        .destroy_instance = destroy_instance,
        .on_midi          = on_midi,
        .set_param        = set_param,
        .get_param        = get_param,
        .get_error        = NULL,
        .render_block     = render_block,
    };
    return &api;
}
