/**
 * Essaim — 32-voice rhythmic oscillator swarm
 * Author: fillioning
 * License: MIT
 *
 * Architecture: 32 independent self-triggering voices, one per pad (notes 36–67).
 * Each voice has randomized SVF mode/Q on load. Pressing a pad toggles its voice
 * on/off and switches the knob overlay to that voice's parameters.
 *
 * API: plugin_api_v2_t
 * Audio: 44100Hz, 128 frames/block, stereo interleaved int16 output
 *
 * Pages:
 *   Global — Root Note, Scale
 *   Voice  — Speed, Mod, Decay, Timbre, Frequency, Noisiness, Cutoff, Volume
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define SAMPLE_RATE  44100.0f
#define N_VOICES     32
#define FIRST_NOTE   36
#define LAST_NOTE    (FIRST_NOTE + N_VOICES - 1)
#define TWO_PI       6.283185307f

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

/* Quantize a continuous semitone offset to the nearest scale degree */
static float quantize_to_scale(float semitones, int root, int scale_idx) {
    if (scale_idx < 0 || scale_idx >= N_SCALES) scale_idx = 0;
    const int *scale = SCALES[scale_idx];
    int len = SCALE_LENS[scale_idx];

    /* Decompose into octave + fractional position */
    int oct = (int)floorf(semitones / 12.0f);
    float rem = semitones - oct * 12.0f;

    /* Find nearest scale degree */
    float best = 999.0f;
    int best_deg = 0;
    for (int i = 0; i < len; i++) {
        float diff = fabsf(rem - scale[i]);
        /* Also check wrapping (e.g., rem=11.5, degree=0 → diff via 12) */
        float diff_wrap = fabsf(rem - (scale[i] + 12));
        if (diff_wrap < diff) { diff = diff_wrap; }
        if (diff < best) { best = diff; best_deg = scale[i]; }
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

/* ── Voice state ───────────────────────────────────────────────────────────── */

typedef struct {
    int   active;          /* voice producing sound */
    float lfo_phase;       /* 0–1 volume LFO phase */
    float env;             /* onset decay envelope 0–1 */
    float osc_phase;       /* oscillator phase 0–1 */

    /* SVF filter state */
    float svf_lp, svf_bp, svf_hp;

    /* Per-voice random (fixed at load) */
    int   svf_mode;        /* 0=LP, 1=BP, 2=HP */
    float svf_q;           /* resonance 0.3–0.95 */
    float pan;             /* -1 to 1 stereo position */
    int   lfo_shape;       /* 0=sine, 1=triangle, 2=saw, 3=square */

    /* User params */
    float speed;           /* retrigger rate Hz */
    float mod;             /* envelope→freq mod depth */
    float decay;           /* envelope decay time (seconds) */
    float timbre;          /* oscillator shape morph 0–1 */
    float frequency;       /* pitch param 0–1 (quantized to scale) */
    float noisiness;       /* noise blend 0–1 */
    float cutoff;          /* SVF cutoff 0–1 */
    float volume;          /* per-voice level 0–1 */
} voice_t;

/* ── Instance state ────────────────────────────────────────────────────────── */

typedef struct {
    voice_t voices[N_VOICES];
    int     current_voice;     /* which voice's knobs are shown (0–31) */
    int     current_page;      /* 0=Global, 1=Voice */

    /* Global params */
    int     root_note;         /* 0–11 (C=0) */
    int     scale;             /* 0–5 */

    /* RNG state */
    uint32_t rng;
} essaim_t;

/* ── RNG ───────────────────────────────────────────────────────────────────── */

static inline uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static inline float rand_float(uint32_t *rng) {
    return (float)(xorshift32(rng) & 0x7FFFFFFF) / 2147483648.0f;
}

static inline float rand_range(uint32_t *rng, float lo, float hi) {
    return lo + rand_float(rng) * (hi - lo);
}

/* ── Oscillator shape morph ────────────────────────────────────────────────── */

static inline float osc_shape(float phase, float timbre) {
    /* timbre 0→0.33: sine→triangle, 0.33→0.66: tri→saw, 0.66→1: saw→square */
    float sine = sinf(phase * TWO_PI);
    float tri  = 4.0f * fabsf(phase - 0.5f) - 1.0f;
    float saw  = 2.0f * phase - 1.0f;
    float sq   = phase < 0.5f ? 1.0f : -1.0f;

    if (timbre < 0.333f) {
        float t = timbre * 3.0f;
        return sine * (1.0f - t) + tri * t;
    } else if (timbre < 0.666f) {
        float t = (timbre - 0.333f) * 3.0f;
        return tri * (1.0f - t) + saw * t;
    } else {
        float t = (timbre - 0.666f) * 3.0f;
        if (t > 1.0f) t = 1.0f;
        return saw * (1.0f - t) + sq * t;
    }
}

/* ── LFO shape (unipolar 0–1, random per voice) ───────────────────────────── */

static inline float lfo_shape(float phase, int shape) {
    switch (shape) {
        case 1:  /* triangle */
            return 1.0f - 2.0f * fabsf(phase - 0.5f);
        case 2:  /* sawtooth (ramp down → sharp attack, slow fade) */
            return 1.0f - phase;
        case 3:  /* square */
            return phase < 0.5f ? 1.0f : 0.0f;
        default: /* sine (unipolar) */
            return 0.5f + 0.5f * sinf(phase * TWO_PI);
    }
}

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    essaim_t *inst = calloc(1, sizeof(essaim_t));
    if (!inst) return NULL;

    /* Seed RNG from pointer address (different per load) */
    inst->rng = (uint32_t)(uintptr_t)inst ^ 0xDEADBEEF;
    xorshift32(&inst->rng); /* warm up */

    inst->root_note = 0;  /* C */
    inst->scale = 0;      /* Chromatic */
    inst->current_voice = 0;
    inst->current_page = 1; /* Start on Voice page */

    for (int i = 0; i < N_VOICES; i++) {
        voice_t *v = &inst->voices[i];
        v->active = 0;
        v->lfo_phase = rand_float(&inst->rng); /* stagger LFOs */
        v->env = 0.0f;
        v->osc_phase = rand_float(&inst->rng);

        /* Random SVF mode and Q — NOT user-controllable */
        v->svf_mode  = (int)(rand_float(&inst->rng) * 3.0f) % 3;
        v->svf_q     = rand_range(&inst->rng, 0.3f, 0.95f);
        v->pan       = rand_range(&inst->rng, -0.7f, 0.7f);
        v->lfo_shape = (int)(rand_float(&inst->rng) * 4.0f) % 4; /* 0=sine,1=tri,2=saw,3=sq */

        /* SVF state */
        v->svf_lp = v->svf_bp = v->svf_hp = 0.0f;

        /* Default params — each voice slightly different */
        v->speed     = rand_range(&inst->rng, 1.0f, 8.0f);
        v->mod       = rand_range(&inst->rng, 0.0f, 0.3f);
        v->decay     = rand_range(&inst->rng, 0.05f, 0.5f);
        v->timbre    = rand_range(&inst->rng, 0.0f, 1.0f);
        v->frequency = rand_range(&inst->rng, 0.2f, 0.8f);
        v->noisiness = rand_range(&inst->rng, 0.0f, 0.15f);
        v->cutoff    = rand_range(&inst->rng, 0.55f, 0.97f);
        v->volume    = rand_range(&inst->rng, 0.4f, 0.8f);
    }

    return inst;
}

static void destroy_instance(void *instance) {
    free(instance);
}

/* ── MIDI ──────────────────────────────────────────────────────────────────── */

static void on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    essaim_t *inst = (essaim_t *)instance;
    if (len < 3) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t note   = msg[1];
    uint8_t vel    = msg[2];

    if (status == 0x90 && vel > 0) {
        /* Note On — toggle voice + switch knob focus */
        if (note >= FIRST_NOTE && note <= LAST_NOTE) {
            int idx = note - FIRST_NOTE;
            inst->current_voice = idx;
            inst->voices[idx].active = !inst->voices[idx].active;
            if (inst->voices[idx].active) {
                /* Start onset ramp (don't reset to avoid click) */
                if (inst->voices[idx].env < 0.01f)
                    inst->voices[idx].env = 0.01f;
            }
        }
    }
    /* Note Off ignored — toggle model, voice stays on until next press */
}

/* ── Parameters ────────────────────────────────────────────────────────────── */

/* Per-voice knob definitions */
typedef struct { const char *key; const char *label; float min, max, step; } knob_def_t;

static const knob_def_t VOICE_KNOBS[8] = {
    {"speed",     "Speed",     0.1f, 40.0f, 0.1f},
    {"mod",       "Mod",       0.0f,  1.0f, 0.01f},
    {"decay",     "Decay",     0.005f,2.0f, 0.005f},
    {"timbre",    "Timbre",    0.0f,  1.0f, 0.01f},
    {"frequency", "Freq",      0.0f,  1.0f, 0.01f},
    {"noisiness", "Noise",     0.0f,  1.0f, 0.01f},
    {"cutoff",    "Cutoff",    0.0f,  1.0f, 0.01f},
    {"volume",    "Volume",    0.0f,  1.0f, 0.01f},
};

static const knob_def_t GLOBAL_KNOBS[2] = {
    {"root_note", "Root",  0.0f, 11.0f, 1.0f},
    {"scale",     "Scale", 0.0f,  5.0f, 1.0f},
};

static float *voice_param_ptr(voice_t *v, int idx) {
    switch (idx) {
        case 0: return &v->speed;
        case 1: return &v->mod;
        case 2: return &v->decay;
        case 3: return &v->timbre;
        case 4: return &v->frequency;
        case 5: return &v->noisiness;
        case 6: return &v->cutoff;
        case 7: return &v->volume;
        default: return NULL;
    }
}

static void set_param(void *instance, const char *key, const char *val) {
    essaim_t *inst = (essaim_t *)instance;
    if (!inst || !key || !val) return;

    /* ── Page tracking ── */
    if (strcmp(key, "_level") == 0) {
        if (strcmp(val, "Global") == 0) inst->current_page = 0;
        else if (strcmp(val, "Voice") == 0) inst->current_page = 1;
        return;
    }

    /* ── Knob adjust ── */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_adjust")) {
        int knob_idx = atoi(key + 5) - 1; /* 1-indexed → 0-indexed */
        int delta = atoi(val);

        if (inst->current_page == 0) {
            /* Global page */
            if (knob_idx == 0) {
                inst->root_note = (int)clampf(inst->root_note + delta, 0, 11);
            } else if (knob_idx == 1) {
                inst->scale = (int)clampf(inst->scale + delta, 0, N_SCALES - 1);
            }
        } else {
            /* Voice page */
            if (knob_idx >= 0 && knob_idx < 8) {
                voice_t *v = &inst->voices[inst->current_voice];
                float *p = voice_param_ptr(v, knob_idx);
                if (p) {
                    *p = clampf(*p + delta * VOICE_KNOBS[knob_idx].step,
                                VOICE_KNOBS[knob_idx].min,
                                VOICE_KNOBS[knob_idx].max);
                }
            }
        }
        return;
    }

    /* ── Direct param sets ── */
    float f = atof(val);
    if (strcmp(key, "root_note") == 0)  { inst->root_note = (int)clampf(f, 0, 11); return; }
    if (strcmp(key, "scale") == 0)      { inst->scale = (int)clampf(f, 0, N_SCALES - 1); return; }

    /* Per-voice params (apply to current voice) */
    voice_t *v = &inst->voices[inst->current_voice];
    if (strcmp(key, "speed") == 0)      { v->speed     = clampf(f, 0.1f, 40.0f); return; }
    if (strcmp(key, "mod") == 0)        { v->mod       = clampf(f, 0, 1); return; }
    if (strcmp(key, "decay") == 0)      { v->decay     = clampf(f, 0.005f, 2.0f); return; }
    if (strcmp(key, "timbre") == 0)     { v->timbre    = clampf(f, 0, 1); return; }
    if (strcmp(key, "frequency") == 0)  { v->frequency = clampf(f, 0, 1); return; }
    if (strcmp(key, "noisiness") == 0)  { v->noisiness = clampf(f, 0, 1); return; }
    if (strcmp(key, "cutoff") == 0)     { v->cutoff    = clampf(f, 0, 1); return; }
    if (strcmp(key, "volume") == 0)     { v->volume    = clampf(f, 0, 1); return; }

    /* ── State deserialization ── */
    if (strcmp(key, "state") == 0) {
        /* Format: root_note scale voice0_params... voice1_params... */
        const char *p = val;
        int rn, sc;
        int consumed = 0;
        if (sscanf(p, "%d %d%n", &rn, &sc, &consumed) == 2) {
            inst->root_note = clampf(rn, 0, 11);
            inst->scale = clampf(sc, 0, N_SCALES - 1);
            p += consumed;
        }
        for (int i = 0; i < N_VOICES; i++) {
            float sp, mo, dc, tb, fq, ns, co, vo;
            int svf_m, lfo_s;
            float svf_r, pn;
            consumed = 0;
            if (sscanf(p, " %f %f %f %f %f %f %f %f %d %f %f %d%n",
                       &sp, &mo, &dc, &tb, &fq, &ns, &co, &vo,
                       &svf_m, &svf_r, &pn, &lfo_s, &consumed) == 12) {
                inst->voices[i].speed     = sp;
                inst->voices[i].mod       = mo;
                inst->voices[i].decay     = dc;
                inst->voices[i].timbre    = tb;
                inst->voices[i].frequency = fq;
                inst->voices[i].noisiness = ns;
                inst->voices[i].cutoff    = co;
                inst->voices[i].volume    = vo;
                inst->voices[i].svf_mode  = svf_m;
                inst->voices[i].svf_q     = svf_r;
                inst->voices[i].pan       = pn;
                inst->voices[i].lfo_shape = lfo_s;
                p += consumed;
            }
        }
    }
}

/* ── ui_hierarchy JSON (must be returned from get_param for sound generators) ── */
static const char UI_HIERARCHY_JSON[] =
    "{\"modes\":null,\"levels\":{"
    "\"root\":{\"name\":\"Essaim\",\"knobs\":[],\"params\":["
      "{\"level\":\"Global\",\"label\":\"Global\"},"
      "{\"level\":\"Voice\",\"label\":\"Voice\"}"
    "]},"
    "\"Global\":{\"name\":\"Global\","
      "\"knobs\":[\"root_note\",\"scale\"],"
      "\"params\":[\"root_note\",\"scale\"]"
    "},"
    "\"Voice\":{\"name\":\"Voice\","
      "\"knobs\":[\"speed\",\"mod\",\"decay\",\"timbre\",\"frequency\",\"noisiness\",\"cutoff\",\"volume\"],"
      "\"params\":[\"speed\",\"mod\",\"decay\",\"timbre\",\"frequency\",\"noisiness\",\"cutoff\",\"volume\"]"
    "}"
    "}}";

/* ── chain_params JSON ── */
static const char CHAIN_PARAMS_JSON[] =
    "["
    "{\"key\":\"root_note\",\"name\":\"Root Note\",\"type\":\"enum\","
      "\"values\":[\"C\",\"C#\",\"D\",\"D#\",\"E\",\"F\",\"F#\",\"G\",\"G#\",\"A\",\"A#\",\"B\"]},"
    "{\"key\":\"scale\",\"name\":\"Scale\",\"type\":\"enum\","
      "\"values\":[\"Chromatic\",\"Major\",\"Minor\",\"Pentatonic\",\"Whole Tone\",\"Harm Minor\"]},"
    "{\"key\":\"speed\",\"name\":\"Speed\",\"type\":\"float\",\"min\":0.1,\"max\":40,\"step\":0.1},"
    "{\"key\":\"mod\",\"name\":\"Mod\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"decay\",\"name\":\"Decay\",\"type\":\"float\",\"min\":0.005,\"max\":2.0,\"step\":0.005},"
    "{\"key\":\"timbre\",\"name\":\"Timbre\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"frequency\",\"name\":\"Freq\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"noisiness\",\"name\":\"Noise\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"cutoff\",\"name\":\"Cutoff\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"volume\",\"name\":\"Volume\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01}"
    "]";

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    essaim_t *inst = (essaim_t *)instance;

    /* ── Framework queries ── */
    if (strcmp(key, "ui_hierarchy") == 0) {
        int len = (int)strlen(UI_HIERARCHY_JSON);
        if (len >= buf_len) len = buf_len - 1;
        memcpy(buf, UI_HIERARCHY_JSON, len);
        buf[len] = '\0';
        return len;
    }

    if (strcmp(key, "chain_params") == 0) {
        int len = (int)strlen(CHAIN_PARAMS_JSON);
        if (len >= buf_len) len = buf_len - 1;
        memcpy(buf, CHAIN_PARAMS_JSON, len);
        buf[len] = '\0';
        return len;
    }

    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "Essaim");

    /* ── Knob overlay (page-aware) ── */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_name")) {
        int idx = atoi(key + 5) - 1;
        if (inst->current_page == 0) {
            /* Global */
            if (idx >= 0 && idx < 2)
                return snprintf(buf, buf_len, "%s", GLOBAL_KNOBS[idx].label);
            return snprintf(buf, buf_len, " ");
        } else {
            /* Voice */
            if (idx >= 0 && idx < 8)
                return snprintf(buf, buf_len, "V%d %s", inst->current_voice + 1, VOICE_KNOBS[idx].label);
        }
        return 0;
    }

    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_value")) {
        int idx = atoi(key + 5) - 1;
        if (inst->current_page == 0) {
            /* Global */
            if (idx == 0) return snprintf(buf, buf_len, "%s", NOTE_NAMES[inst->root_note]);
            if (idx == 1) return snprintf(buf, buf_len, "%s", SCALE_NAMES[inst->scale]);
            return snprintf(buf, buf_len, " ");
        } else {
            /* Voice */
            if (idx >= 0 && idx < 8) {
                voice_t *v = &inst->voices[inst->current_voice];
                float *p = voice_param_ptr(v, idx);
                if (p) {
                    if (idx == 0) return snprintf(buf, buf_len, "%.1fHz", *p);
                    return snprintf(buf, buf_len, "%d%%", (int)(*p * 100));
                }
            }
        }
        return 0;
    }

    /* ── Global params ── */
    if (strcmp(key, "root_note") == 0) return snprintf(buf, buf_len, "%s", NOTE_NAMES[inst->root_note]);
    if (strcmp(key, "scale") == 0)     return snprintf(buf, buf_len, "%s", SCALE_NAMES[inst->scale]);

    /* ── Per-voice params (current voice) ── */
    voice_t *v = &inst->voices[inst->current_voice];
    if (strcmp(key, "speed") == 0)     return snprintf(buf, buf_len, "%.4f", v->speed);
    if (strcmp(key, "mod") == 0)       return snprintf(buf, buf_len, "%.4f", v->mod);
    if (strcmp(key, "decay") == 0)     return snprintf(buf, buf_len, "%.4f", v->decay);
    if (strcmp(key, "timbre") == 0)    return snprintf(buf, buf_len, "%.4f", v->timbre);
    if (strcmp(key, "frequency") == 0) return snprintf(buf, buf_len, "%.4f", v->frequency);
    if (strcmp(key, "noisiness") == 0) return snprintf(buf, buf_len, "%.4f", v->noisiness);
    if (strcmp(key, "cutoff") == 0)    return snprintf(buf, buf_len, "%.4f", v->cutoff);
    if (strcmp(key, "volume") == 0)    return snprintf(buf, buf_len, "%.4f", v->volume);

    /* ── State serialization ── */
    if (strcmp(key, "state") == 0) {
        int pos = snprintf(buf, buf_len, "%d %d", inst->root_note, inst->scale);
        for (int i = 0; i < N_VOICES && pos < buf_len - 1; i++) {
            voice_t *vi = &inst->voices[i];
            pos += snprintf(buf + pos, buf_len - pos,
                " %f %f %f %f %f %f %f %f %d %f %f %d",
                vi->speed, vi->mod, vi->decay, vi->timbre,
                vi->frequency, vi->noisiness, vi->cutoff, vi->volume,
                vi->svf_mode, vi->svf_q, vi->pan, vi->lfo_shape);
        }
        return pos;
    }

    return -1; /* unknown key */
}

/* ── Audio processing ──────────────────────────────────────────────────────── */

static void render_block(void *instance, int16_t *out_lr, int frames) {
    essaim_t *inst = (essaim_t *)instance;

    for (int i = 0; i < frames; i++) {
        float mix_l = 0.0f;
        float mix_r = 0.0f;

        for (int vi = 0; vi < N_VOICES; vi++) {
            voice_t *v = &inst->voices[vi];
            if (!v->active && v->env < 0.001f) continue;

            /* ── Volume LFO (rhythmic character) ── */
            v->lfo_phase += v->speed / SAMPLE_RATE;
            if (v->lfo_phase >= 1.0f) v->lfo_phase -= 1.0f;
            float lfo_val = lfo_shape(v->lfo_phase, v->lfo_shape);

            /* ── Onset/release envelope (exponential) ── */
            if (v->active) {
                /* Attack: fast rise to 1.0 */
                v->env += (1.05f - v->env) * 0.05f;
            } else {
                /* Release: exponential decay out */
                float decay_rate = 1.0f / (v->decay * SAMPLE_RATE + 1.0f);
                v->env *= (1.0f - decay_rate);
                if (v->env < 0.0001f) { v->env = 0.0f; continue; }
            }

            /* ── Frequency (quantized to scale) ── */
            float semitones = v->frequency * 48.0f; /* 0–1 → 0–48 semitones */
            float base_freq = quantize_to_scale(semitones, inst->root_note + 48, inst->scale);

            /* LFO → FM (mod param) */
            float fm = 1.0f + lfo_val * v->mod * 2.0f;
            float freq = base_freq * fm;

            /* ── Oscillator ── */
            v->osc_phase += freq / SAMPLE_RATE;
            if (v->osc_phase >= 1.0f) v->osc_phase -= 1.0f;
            float osc = osc_shape(v->osc_phase, v->timbre);

            /* ── Noise ── */
            float noise = (float)(int32_t)xorshift32(&inst->rng) / 2147483648.0f;

            /* Equal-power crossfade */
            float nm = v->noisiness * 0.5f; /* scale to 50% max */
            float sig = osc * (1.0f - nm) + noise * nm;

            /* ── SVF filter ── */
            float cutoff_shaped = v->cutoff * v->cutoff;
            float fc = 30.0f * powf(600.0f, cutoff_shaped); /* 30–18000 Hz */
            float f_coeff = 2.0f * sinf(3.14159f * fc / SAMPLE_RATE);
            if (f_coeff > 1.8f) f_coeff = 1.8f; /* stability */
            float q_fb = 1.0f - v->svf_q;

            v->svf_hp = sig - v->svf_lp - q_fb * v->svf_bp;
            v->svf_bp += f_coeff * v->svf_hp;
            v->svf_lp += f_coeff * v->svf_bp;

            float filtered;
            switch (v->svf_mode) {
                case 1:  filtered = v->svf_bp; break;
                case 2:  filtered = v->svf_hp; break;
                default: filtered = v->svf_lp; break;
            }

            /* ── Apply LFO × envelope × volume ── */
            float out = filtered * lfo_val * v->env * v->volume;

            /* ── Stereo pan (constant power) ── */
            float pan01 = (v->pan + 1.0f) * 0.5f;
            mix_l += out * cosf(pan01 * 1.5708f);
            mix_r += out * sinf(pan01 * 1.5708f);
        }

        /* ── Master soft clip ── */
        float scale = 0.15f; /* headroom for 32 voices */
        mix_l = fast_tanh(mix_l * scale) * 0.95f;
        mix_r = fast_tanh(mix_r * scale) * 0.95f;

        int16_t sl = (int16_t)(clampf(mix_l, -1.0f, 1.0f) * 32767.0f);
        int16_t sr = (int16_t)(clampf(mix_r, -1.0f, 1.0f) * 32767.0f);
        out_lr[i * 2]     = sl;
        out_lr[i * 2 + 1] = sr;
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
