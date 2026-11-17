/* SPDX-License-Identifier: MIT
 * NOOK: lo-fi jam companion for the M-VAVE FM-1. */
/* The band: see lofi.h. Everything here runs in the audio interrupt (eng_render), except eng_init. Floats only
 * (the FM-1's FPU is single precision), no libm: fastmath.h and a sine table. */
#include "lofi.h"
#include "music.h"
#include "fastmath.h"
#include "plat.h"

#define SR ENG_SR
volatile eng_ctl_t eng_ctl;
volatile eng_stat_t eng_stat;

/* ------------------------------------------------------------ helpers --- */
#define SINE_N 1024
static float sine_t[SINE_N + 1];
static inline float sin_t(float ph)                  /* ph in turns, any sign */
{
    float x;
    int32_t i;
    ph -= (float)(int32_t)ph;
    if (ph < 0.0f)
        ph += 1.0f;
    x = ph * (float)SINE_N;
    i = (int32_t)x;
    if (i >= SINE_N)
        i = SINE_N - 1;
    x -= (float)i;
    return sine_t[i] + (sine_t[i + 1] - sine_t[i]) * x;
}
static inline float wrap1(float p) { return p >= 1.0f ? p - (float)(int32_t)p : p; }

static uint32_t rn_noise = 0x1234567u, rn_free = 0xC0FFEEu, rn_det = 1u;
static inline uint32_t xs(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}
static inline float frand(uint32_t *s) { return (float)(xs(s) >> 8) * (1.0f / 16777216.0f); }   /* 0..1 */
static inline float noise(void) { return (float)(int32_t)xs(&rn_noise) * (1.0f / 2147483648.0f); }   /* -1..1 */

/* the generative choices: the loop's own random (seeded per bar of the loop, so with VARIATION at 0 every pass
 * of the loop is the same), or a fresh one, as often as VARIATION says */
static float var_now;
static inline float gr(void)
{
    float d = frand(&rn_det);
    if (var_now > 0.0f && frand(&rn_free) < var_now)
        return frand(&rn_free);
    return d;
}
static inline int gri(int n) { int v = (int)(gr() * (float)n); return v >= n ? n - 1 : v; }

static inline float mtof(float n) { return 440.0f * fm_exp2f((n - 69.0f) * (1.0f / 12.0f)); }
static inline float tau_k(float s) { return fm_expf(-1.0f / (s * SR)); }   /* per-sample decay for a time constant */
static inline float lp_k(float hz) { return 1.0f - fm_expf(-FM_TWO_PI * hz / SR); }   /* one-pole low-pass */
static inline float softclip(float x)                /* tanh, near enough, clamped */
{
    float y;
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    y = x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
    return y;
}

/* --------------------------------------------------------- the keys --- */
typedef struct {
    float ratio, idx, idx_dec, idx_sus;  /* the modulator: ratio, index (turns), its decay (s), what it keeps */
    float bell, bell_dec;                /* the tine: a high partial at the attack */
    float atk, dec, rel;                 /* amp: attack (s), decay time constant (s), release (s) */
    float det;                           /* a second carrier, detuned (cents): chorus; 0 = one */
    float bright;                        /* the voice's low-pass, Hz at full velocity */
    float trem, trem_hz;                 /* tremolo depth, rate */
    uint8_t trem_pan;                    /* the tremolo pans (a Rhodes' stereo) rather than pulses (a Wurli) */
    float level;
} ksound_t;
static const ksound_t KSOUNDS[NKSOUND] = {
    /* RHODES */ {1.0f, 0.30f, 0.55f, 0.18f, 0.22f, 0.045f, 0.002f, 2.4f, 0.22f, 2.5f, 5200.0f, 0.22f, 4.3f, 1, 0.30f},
    /* FELT */   {1.0f, 0.24f, 0.30f, 0.12f, 0.06f, 0.020f, 0.004f, 1.5f, 0.40f, 1.5f, 3800.0f, 0.00f, 4.0f, 0, 0.34f},
    /* WURLI */  {1.0f, 0.42f, 0.28f, 0.32f, 0.00f, 0.020f, 0.002f, 1.7f, 0.14f, 0.0f, 4200.0f, 0.28f, 5.6f, 0, 0.24f},
    /* DREAM */  {2.0f, 0.12f, 1.40f, 0.30f, 0.12f, 0.080f, 0.045f, 4.5f, 0.90f, 7.0f, 3600.0f, 0.18f, 2.2f, 1, 0.28f},
};

#define NKV 12
typedef struct {
    float c1, c2, m, b;                  /* phases */
    float inc1, inc2, minc, binc;
    float amp, a_inc, kdec, krel;
    float ie, kie, be, kbe;
    float idx, isus, bell;
    float lp, lpk;
    float gl, gr;
    uint8_t stage;                       /* 0 off, 1 attack, 2 held (decaying), 3 released */
    uint8_t note;
    uint16_t tag;
    uint32_t age;
} kvoice_t;
static kvoice_t kv[NKV];
static const ksound_t *ks = &KSOUNDS[0];
static float k_kdec, k_krel, k_kie, k_kbe, k_ainc;  /* the sound's coefficients */
static uint32_t kv_age;

static void ksound_set(int s)
{
    ks = &KSOUNDS[s < 0 || s >= NKSOUND ? 0 : s];
    k_kdec = tau_k(ks->dec);
    k_krel = tau_k(ks->rel * 0.4f);
    k_kie = tau_k(ks->idx_dec);
    k_kbe = tau_k(ks->bell_dec);
    k_ainc = 1.0f / (ks->atk * SR);
}

static void key_on(int note, int vel, uint16_t tag)
{
    int i, best = 0;
    float v = (float)vel * (1.0f / 127.0f), f = mtof((float)note), lowest = 10.0f, pan;
    kvoice_t *o;
    for (i = 0; i < NKV; i++)            /* a free voice, or the quietest released, or the quietest */
        if (kv[i].stage == 0) {
            best = i;
            lowest = -1.0f;
            break;
        }
    if (lowest > 0.0f)
        for (i = 0; i < NKV; i++) {
            float l = kv[i].amp * (kv[i].stage == 3 ? 0.3f : 1.0f);
            if (l < lowest) {
                lowest = l;
                best = i;
            }
        }
    o = &kv[best];
    o->inc1 = f / SR;
    o->inc2 = ks->det > 0.0f ? o->inc1 * fm_exp2f(ks->det * (1.0f / 1200.0f)) : 0.0f;
    o->minc = o->inc1 * ks->ratio;
    o->binc = f * 14.0f < 17000.0f ? o->inc1 * 14.0f : 0.0f;
    if (o->stage == 0) {
        o->c1 = o->c2 = o->m = o->b = 0.0f;
        o->amp = 0.0f;
        o->lp = 0.0f;
    }
    o->a_inc = k_ainc;
    o->kdec = k_kdec * (note > 72 ? 0.99999f : 1.0f);   /* the top a little shorter */
    o->krel = k_krel;
    o->ie = 1.0f;
    o->kie = k_kie;
    o->be = 1.0f;
    o->kbe = k_kbe;
    o->idx = ks->idx * (0.45f + 0.75f * v);
    o->isus = ks->idx_sus;
    o->bell = ks->bell * v;
    o->lpk = lp_k(ks->bright * (0.35f + 0.65f * v) * (note < 55 ? 0.8f : 1.0f));
    pan = ((float)note - 64.0f) * 0.018f;
    pan = fm_clampf(pan, -0.35f, 0.35f);
    o->gl = (0.5f - pan * 0.5f) * v * ks->level * 2.0f;
    o->gr = (0.5f + pan * 0.5f) * v * ks->level * 2.0f;
    o->stage = 1;
    o->note = (uint8_t)note;
    o->tag = tag;
    o->age = ++kv_age;
}

static void key_off(uint16_t tag)
{
    int i;
    for (i = 0; i < NKV; i++)
        if (kv[i].stage && kv[i].stage < 3 && kv[i].tag == tag)
            kv[i].stage = 3;
}

static inline void keys_voice(kvoice_t *o, float *l, float *r)
{
    float mod, s, idx;
    if (o->stage == 1) {
        o->amp += o->a_inc;
        if (o->amp >= 1.0f) {
            o->amp = 1.0f;
            o->stage = 2;
        }
    } else if (o->stage == 2) {
        o->amp *= o->kdec;
    } else {
        o->amp *= o->krel;
    }
    if (o->amp < 0.0002f && o->stage >= 2) {
        o->stage = 0;
        return;
    }
    o->ie *= o->kie;
    idx = o->idx * (o->isus + (1.0f - o->isus) * o->ie);
    o->m = wrap1(o->m + o->minc);
    mod = sin_t(o->m) * idx;
    o->c1 = wrap1(o->c1 + o->inc1);
    s = sin_t(o->c1 + mod);
    if (o->inc2 > 0.0f) {
        o->c2 = wrap1(o->c2 + o->inc2);
        s = 0.6f * (s + sin_t(o->c2 + mod));
    }
    if (o->binc > 0.0f && o->be > 0.001f) {
        o->be *= o->kbe;
        o->b = wrap1(o->b + o->binc);
        s += sin_t(o->b) * o->bell * o->be;
    }
    o->lp += (s - o->lp) * o->lpk;
    s = o->lp * o->amp;
    *l += s * o->gl;
    *r += s * o->gr;
}

/* ---------------------------------------------------------- the bass --- */
static struct {
    float ph, inc, inc_to, amp, target, lp, lpk;
    float katk, kdec, krel;
    uint8_t on, note;
    uint16_t tag;
} bs;

static void bass_on(int note, int vel, uint16_t tag)
{
    float f = mtof((float)note);
    bs.inc_to = f / SR;
    if (!bs.on || bs.amp < 0.05f)
        bs.inc = bs.inc_to;              /* (legato: it glides) */
    bs.target = 0.55f + 0.45f * (float)vel * (1.0f / 127.0f);
    bs.on = 1;
    bs.note = (uint8_t)note;
    bs.tag = tag;
    if (bs.amp > 0.7f * bs.target)
        bs.amp = 0.7f * bs.target;       /* a little dip: each note speaks */
    eng_stat.bass_note = note;
    eng_stat.bassnotes++;
}
static void bass_off(uint16_t tag)
{
    if (bs.on && bs.tag == tag) {
        bs.on = 0;
        eng_stat.bass_note = -1;
    }
}
static inline float bass_sample(void)
{
    float s;
    if (bs.on)
        bs.amp += (bs.target - bs.amp) * (bs.amp < bs.target ? bs.katk : 0.00003f);
    else
        bs.amp *= bs.krel;
    if (bs.amp < 0.00005f)
        return bs.lp = fm_flush(bs.lp * 0.99f);
    bs.inc += (bs.inc_to - bs.inc) * 0.004f;
    bs.ph = wrap1(bs.ph + bs.inc);
    s = sin_t(bs.ph) + 0.22f * sin_t(bs.ph * 2.0f) + 0.08f * sin_t(bs.ph * 3.0f);
    s = softclip(s * 1.3f) * bs.amp;
    bs.lp += (s - bs.lp) * bs.lpk;
    return bs.lp;
}

/* ------------------------------------------------------------ the drums --- */
typedef struct {
    float k_f0, k_f1, k_pdec, k_dec, k_click, k_drive;
    float s_f, s_tone, s_noise, s_ndec, s_lp;
    uint8_t clap;
    float h_hp, h_cdec, h_odec, h_lvl;
    int32_t hold;                        /* sample-and-hold: the sampler's rate */
    float bits;                          /* its depth */
    float bus_lp;
} kit_t;
static const kit_t KITS[NKIT] = {
    /* DUSTY */ {118.0f, 47.0f, 0.034f, 0.38f, 0.30f, 1.7f, 190.0f, 0.50f, 0.70f, 0.15f, 6000.0f, 0,
                 7000.0f, 0.035f, 0.26f, 0.32f, 3, 11.0f, 7600.0f},
    /* WARM */  {100.0f, 45.0f, 0.045f, 0.44f, 0.15f, 1.3f, 175.0f, 0.30f, 0.62f, 0.19f, 4600.0f, 1,
                 6500.0f, 0.032f, 0.30f, 0.26f, 2, 12.0f, 6200.0f},
    /* CRISP */ {140.0f, 52.0f, 0.030f, 0.32f, 0.45f, 1.9f, 210.0f, 0.45f, 0.90f, 0.13f, 9500.0f, 0,
                 8000.0f, 0.030f, 0.22f, 0.38f, 1, 13.0f, 11000.0f},
    /* SOFT */  {90.0f, 44.0f, 0.050f, 0.50f, 0.05f, 1.1f, 160.0f, 0.15f, 0.55f, 0.27f, 3600.0f, 0,
                 6000.0f, 0.040f, 0.34f, 0.22f, 2, 12.0f, 5200.0f},
};
enum { DR_KICK, DR_SNARE, DR_HAT, DR_OHAT, DR_RIM, DR_SHAKER, NDR };
static const kit_t *kit = &KITS[0];
static struct {
    float kph, kf, kpe, ka, kclick, kv;
    float kpek, kak;
    float sph, sa, sn, sn_lp, sn_hp, sa_k, sn_k, s_lpk, clap_t;
    float ha, ha_k, h_x1, h_y1, h_y2, h_lp;
    float rph1, rph2, ra;
    float pa, p_atk, p_y, p_lp;
    float hold_l, hold_r, bus_l, bus_r, bus_k;
    int32_t hold_n;
} dr;
static float kit_hp_k, kit_slp_k;

static void kit_set(int k)
{
    kit = &KITS[k < 0 || k >= NKIT ? 0 : k];
    dr.kpek = tau_k(kit->k_pdec);
    dr.kak = tau_k(kit->k_dec);
    dr.sa_k = tau_k(0.06f);
    dr.sn_k = tau_k(kit->s_ndec);
    kit_slp_k = lp_k(kit->s_lp);
    kit_hp_k = fm_expf(-FM_TWO_PI * kit->h_hp / SR);
    dr.bus_k = lp_k(kit->bus_lp);
}

static void drum_hit(int d, float v)
{
    switch (d) {
    case DR_KICK:
        dr.kph = 0.0f;
        dr.kpe = 1.0f;
        dr.ka = v;
        dr.kv = v;
        dr.kclick = v * kit->k_click;
        eng_stat.kicks++;
        break;
    case DR_SNARE:
        dr.sph = 0.0f;
        dr.sa = v * kit->s_tone;
        dr.sn = v * kit->s_noise;
        dr.clap_t = kit->clap ? 0.0f : -1.0f;
        if (v > 0.5f)
            eng_stat.snares++;
        break;
    case DR_HAT:
    case DR_OHAT:
        dr.ha = v * kit->h_lvl;
        dr.ha_k = tau_k(d == DR_OHAT ? kit->h_odec : kit->h_cdec);
        eng_stat.hats++;
        break;
    case DR_RIM:
        dr.rph1 = dr.rph2 = 0.0f;
        dr.ra = v * 0.5f;
        if (v > 0.5f)
            eng_stat.snares++;
        break;
    case DR_SHAKER:
        dr.pa = 0.0f;
        dr.p_atk = v * 0.16f;
        break;
    }
}

static inline void drums_sample(float *l, float *r)
{
    float k = 0.0f, s = 0.0f, h = 0.0f, rim = 0.0f, p = 0.0f, n, x;
    /* kick: a falling sine and a click */
    if (dr.ka > 0.0005f) {
        dr.kf = kit->k_f1 + (kit->k_f0 - kit->k_f1) * dr.kpe;
        dr.kpe *= dr.kpek;
        dr.kph = wrap1(dr.kph + dr.kf / SR);
        k = softclip(sin_t(dr.kph) * kit->k_drive) * dr.ka;
        dr.ka *= dr.kak;
        if (dr.kclick > 0.001f) {
            k += noise() * dr.kclick;
            dr.kclick *= 0.86f;
        }
    }
    /* snare: a tone and a band of noise (the clap: three quick bursts first) */
    if (dr.sa > 0.0005f || dr.sn > 0.0005f) {
        n = noise();
        dr.sn_lp += (n - dr.sn_lp) * kit_slp_k;
        x = dr.sn_lp - dr.sn_hp;
        dr.sn_hp += (dr.sn_lp - dr.sn_hp) * 0.06f;
        if (dr.clap_t >= 0.0f) {
            float t = dr.clap_t;
            float g = t < 0.011f ? 1.0f - t * 70.0f : t < 0.022f ? 1.0f - (t - 0.011f) * 70.0f :
                      t < 0.033f ? 1.0f - (t - 0.022f) * 60.0f : 1.0f;
            dr.clap_t += 1.0f / SR;
            if (dr.clap_t > 0.04f)
                dr.clap_t = -1.0f;
            x *= g;
        }
        dr.sph = wrap1(dr.sph + kit->s_f / SR);
        s = sin_t(dr.sph) * dr.sa + x * dr.sn * 1.6f;
        dr.sa *= dr.sa_k;
        if (dr.clap_t < 0.0f)
            dr.sn *= dr.sn_k;
    }
    /* hats: white noise through two high-passes */
    if (dr.ha > 0.0005f) {
        float y1;
        n = noise();
        y1 = kit_hp_k * (dr.h_y1 + n - dr.h_x1);
        dr.h_x1 = n;
        dr.h_y2 = kit_hp_k * (dr.h_y2 + y1 - dr.h_y1);
        dr.h_y1 = y1;
        h = dr.h_y2 * dr.ha;
        dr.ha *= dr.ha_k;
    }
    /* rim: two short tones */
    if (dr.ra > 0.0005f) {
        dr.rph1 = wrap1(dr.rph1 + 820.0f / SR);
        dr.rph2 = wrap1(dr.rph2 + 1690.0f / SR);
        rim = (sin_t(dr.rph1) * 0.7f + sin_t(dr.rph2) * 0.4f) * dr.ra;
        dr.ra *= 0.9975f;
    }
    /* shaker: noise, high, a soft swell and a quick fall */
    if (dr.p_atk > 0.0f || dr.pa > 0.0005f) {
        if (dr.p_atk > 0.0f) {
            dr.pa += 0.0035f;
            if (dr.pa >= dr.p_atk) {
                dr.pa = dr.p_atk;
                dr.p_atk = 0.0f;
            }
        } else {
            dr.pa *= 0.9993f;
        }
        n = noise();
        dr.p_lp += (n - dr.p_lp) * 0.5f;
        p = (n - dr.p_lp) * dr.pa;
    }
    *l = k + s + h * 0.8f + rim * 1.1f + p * 1.3f;
    *r = k + s + h * 1.2f + rim * 0.9f + p * 0.7f;
}

/* ---------------------------------------------------------- the tape --- */
#define WOW_N 2048
static float wow_l[WOW_N] PLAT_POOL, wow_r[WOW_N] PLAT_POOL;
static uint32_t wow_w;
static float wow_ph, flut_ph, wow_drift, wow_drift_to;

/* reverb: four combs and two all-passes a side (Schroeder / Freeverb's shape) */
#define C0 1116
#define C1 1188
#define C2 1277
#define C3 1356
#define A0 556
#define A1 441
#define RS 23
static float cb_l0[C0] PLAT_POOL, cb_l1[C1] PLAT_POOL, cb_l2[C2] PLAT_POOL, cb_l3[C3] PLAT_POOL;
static float cb_r0[C0 + RS] PLAT_POOL, cb_r1[C1 + RS] PLAT_POOL, cb_r2[C2 + RS] PLAT_POOL, cb_r3[C3 + RS] PLAT_POOL;
static float ap_l0[A0] PLAT_POOL, ap_l1[A1] PLAT_POOL, ap_r0[A0 + RS] PLAT_POOL, ap_r1[A1 + RS] PLAT_POOL;
typedef struct {
    float *b;
    int32_t n, i;
    float fs;
} comb_t;
static comb_t cmb[8];
typedef struct {
    float *b;
    int32_t n, i;
} ap_t;
static ap_t aps[4];
static inline float comb(comb_t *c, float x, float fb, float damp)
{
    float y = c->b[c->i];
    c->fs = y + (c->fs - y) * damp;
    c->b[c->i] = x + c->fs * fb;
    if (++c->i >= c->n)
        c->i = 0;
    return y;
}
static inline float allp(ap_t *a, float x)
{
    float y = a->b[a->i];
    a->b[a->i] = x + y * 0.5f;
    if (++a->i >= a->n)
        a->i = 0;
    return y - x;
}

/* ----------------------------------------------------- the sequencer --- */
enum { EV_NONE, EV_KON, EV_KOFF, EV_BON, EV_BOFF, EV_DRUM };
#define NEV 96
typedef struct {
    uint32_t at;
    uint8_t type, note, vel;
    uint16_t tag;
} ev_t;
static ev_t evq[NEV];
static uint32_t t_now, next_due = 0xFFFFFFFFu;
static uint16_t tag_n;

static void midi3(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (!eng_ctl.midi_out)
        return;
    plat_midi_send((st >> 4) | st << 8 | d1 << 16 | d2 << 24, TO_USB);
}
static void midi_rt(uint32_t b)
{
    if (eng_ctl.midi_out)
        plat_midi_send(0x0Fu | b << 8, TO_USB);
}

static void ev_add(uint32_t delay, int type, int note, int vel, uint16_t tag)
{
    int i;
    for (i = 0; i < NEV; i++)
        if (evq[i].type == EV_NONE) {
            evq[i].at = t_now + delay;
            evq[i].type = (uint8_t)type;
            evq[i].note = (uint8_t)note;
            evq[i].vel = (uint8_t)vel;
            evq[i].tag = tag;
            if ((int32_t)(evq[i].at - next_due) < 0 || next_due == 0xFFFFFFFFu)
                next_due = evq[i].at;
            return;
        }
}

static const uint8_t GM_DRUM[NDR] = {36, 38, 42, 46, 37, 70};
static float lvl_drums, lvl_bass, lvl_keys;
static float duck;

static void run_events(void)
{
    int i;
    uint32_t nd = 0xFFFFFFFFu;
    for (i = 0; i < NEV; i++) {
        ev_t *e = &evq[i];
        if (e->type == EV_NONE)
            continue;
        if ((int32_t)(e->at - t_now) <= 0) {
            switch (e->type) {
            case EV_KON:
                key_on(e->note, e->vel, e->tag);
                eng_stat.keyhits++;
                if (plat_midi_out_room(TO_USB) > 12u)
                    midi3(0x90u, e->note, e->vel);
                break;
            case EV_KOFF:
                key_off(e->tag);
                midi3(0x80u, e->note, 0);
                break;
            case EV_BON:
                bass_on(e->note, e->vel, e->tag);
                if (plat_midi_out_room(TO_USB) > 12u)
                    midi3(0x91u, e->note, e->vel);
                break;
            case EV_BOFF:
                bass_off(e->tag);
                midi3(0x81u, e->note, 0);
                break;
            case EV_DRUM: {
                float v = (float)e->vel * (1.0f / 127.0f);
                drum_hit(e->note, v);
                if (e->note == DR_KICK)
                    duck = v;
                if (plat_midi_out_room(TO_USB) > 12u) {
                    midi3(0x99u, GM_DRUM[e->note], e->vel);
                    midi3(0x89u, GM_DRUM[e->note], 0);
                }
                break;
            }
            }
            e->type = EV_NONE;
        } else if (nd == 0xFFFFFFFFu || (int32_t)(e->at - nd) < 0) {
            nd = e->at;
        }
    }
    next_due = nd;
}

/* what is playing (taken from eng_ctl at a bar line) */
static int cur_mood = 0, cur_root = 9, cur_pat = 0, cur_kstyle = KST_COMP, cur_bstyle = BST_BOUNCE, cur_ksound = -1;
static int playing, step = -1;
static uint32_t bar;
static float step_left, clk_left, sps = 8000.0f, swing_now;
static float bpm_now = 80.0f, density_now = 0.5f;
static int voicing[6], nvoice, prev_voicing[6], nprev, chord_root_pc, chord_q;

/* the bar's plan: piano hits and notes, bass notes */
enum { PK_CHORD, PK_NOTE };
typedef struct {
    uint8_t step, kind, note, len, vel, strum;
} kplan_t;
typedef struct {
    uint8_t step, note, len, vel;
} bplan_t;
static kplan_t kplan[24];
static int nkplan;
static bplan_t bplan[12];
static int nbplan;
static uint8_t dplan[NDR][16];           /* drum velocities this bar (0 none) */

static int pc_of(int n) { return ((n % 12) + 12) % 12; }

/* the chord's notes for the piano: its tones above the root (the bass has the root), in the middle of the
 * keyboard, each chord close to the last one (voice leading); now and then spread a little wider */
static void voice_chord(void)
{
    const quality_t *q = &QUALS[chord_q];
    int pcs[6], np = 0, i, inv, best[6], nbest = 0;
    int32_t best_cost = 0x7FFFFFFF;
    for (i = 1; i < q->n; i++)           /* rootless: the bass has it */
        pcs[np++] = pc_of(chord_root_pc + q->iv[i]);
    if (np < 3)
        pcs[np++] = chord_root_pc;
    for (inv = 0; inv < np; inv++) {
        int notes[6], n = 0, lo;
        int32_t cost = 0;
        for (lo = 52; lo <= 64; lo++) {  /* the bottom note: this inversion's lowest pitch class, 52..64 */
            if (pc_of(lo) == pcs[inv])
                break;
        }
        notes[n++] = lo;
        for (i = 1; i < np; i++) {
            int p = pcs[(inv + i) % np], x = notes[n - 1] + 1;
            while (pc_of(x) != p)
                x++;
            notes[n++] = x;
        }
        if (notes[n - 1] > 79)
            cost += 40;
        if (nprev) {
            for (i = 0; i < n; i++) {
                int j, d = 99;
                for (j = 0; j < nprev; j++) {
                    int dd = notes[i] - prev_voicing[j];
                    if (dd < 0)
                        dd = -dd;
                    if (dd < d)
                        d = dd;
                }
                cost += d * 4;
            }
        } else {
            cost += (notes[0] - 57) * (notes[0] - 57);
        }
        cost += (int32_t)(gr() * 6.0f);  /* a little choice among the near ones */
        if (cost < best_cost) {
            best_cost = cost;
            nbest = n;
            for (i = 0; i < n; i++)
                best[i] = notes[i];
        }
    }
    if (nbest >= 4 && gr() < 0.2f + 0.3f * var_now)   /* drop 2: the second from the top an octave down */
        best[nbest - 2] -= 12;
    for (i = 0; i < nbest; i++) {
        int j;
        for (j = i + 1; j < nbest; j++)
            if (best[j] < best[i]) {
                int t = best[i];
                best[i] = best[j];
                best[j] = t;
            }
    }
    nvoice = nbest;
    for (i = 0; i < nbest; i++)
        voicing[i] = prev_voicing[i] = best[i];
    nprev = nbest;
    eng_stat.top_note = voicing[nvoice - 1];
}

static int bass_of(int pc) { return 28 + pc_of(pc - 4); }   /* E1 .. D#2 */

/* a scale note near n (the mood's scale in the current key) */
static int scale_near(int n, int dir)
{
    const scale_t *s = &SCALES[MOODS[cur_mood].scale];
    int x = n, k;
    for (k = 0; k < 13; k++) {
        int i, p = pc_of(x - cur_root);
        for (i = 0; i < s->n; i++)
            if (s->iv[i] == p)
                return x;
        x += dir;
    }
    return n;
}

static void kp_add(int st, int kind, int note, int len, int vel, int strum)
{
    if (nkplan >= 24 || st > 15)
        return;
    kplan[nkplan].step = (uint8_t)st;
    kplan[nkplan].kind = (uint8_t)kind;
    kplan[nkplan].note = (uint8_t)note;
    kplan[nkplan].len = (uint8_t)(len < 1 ? 1 : len);
    kplan[nkplan].vel = (uint8_t)clamp(vel, 1, 127);
    kplan[nkplan].strum = (uint8_t)strum;
    nkplan++;
}
static void bp_add(int st, int note, int len, int vel)
{
    if (nbplan >= 12 || st > 15)
        return;
    bplan[nbplan].step = (uint8_t)st;
    bplan[nbplan].note = (uint8_t)note;
    bplan[nbplan].len = (uint8_t)(len < 1 ? 1 : len);
    bplan[nbplan].vel = (uint8_t)clamp(vel, 1, 127);
    nbplan++;
}

/* comping: hits (step, length, velocity), sparse to busy */
static const uint8_t COMP[6][4][3] = {
    {{0, 6, 92}, {10, 4, 70}, {0, 0, 0}, {0, 0, 0}},
    {{0, 3, 90}, {6, 2, 66}, {10, 5, 76}, {0, 0, 0}},
    {{0, 7, 88}, {7, 2, 60}, {12, 4, 72}, {0, 0, 0}},
    {{2, 2, 76}, {6, 2, 70}, {10, 2, 74}, {14, 2, 66}},
    {{0, 2, 90}, {3, 3, 70}, {8, 2, 82}, {11, 4, 68}},
    {{0, 4, 86}, {6, 1, 58}, {7, 3, 72}, {14, 2, 64}},
};

static void plan_keys(int kstyle)
{
    int i, top = voicing[nvoice - 1], d10 = (int)(density_now * 10.0f + 0.5f);
    nkplan = 0;
    switch (kstyle) {
    case KST_PADS:
        kp_add(0, PK_CHORD, 0, 16, 84, 1);
        if (gr() < 0.2f + density_now * 0.5f)
            kp_add(gr() < 0.5f ? 8 : 10, PK_CHORD, 0, 6, 58, 2);
        break;
    case KST_STRUM:
        kp_add(0, PK_CHORD, 0, gr() < density_now ? 10 : 16, 92, 4);
        if (gr() < density_now)
            kp_add(gr() < 0.5f ? 10 : 11, PK_CHORD, 0, 6, 66, 3);
        break;
    case KST_COMP: {
        int t = gri(3) + (density_now > 0.66f ? 3 : density_now > 0.33f ? 1 : 0);
        if (t > 5)
            t = 5;
        for (i = 0; i < 4; i++)
            if (COMP[t][i][1])
                kp_add(COMP[t][i][0], PK_CHORD, 0, COMP[t][i][1], COMP[t][i][2] - 8 + gri(16), 1);
        break;
    }
    case KST_ARP: {
        int seq[8], n = 0, dir = gri(4), rate = density_now > 0.7f ? 1 : 2, s;
        for (i = 0; i < nvoice && n < 8; i++)
            seq[n++] = voicing[i];
        if (n < 8)
            seq[n++] = voicing[0] + 12;
        if (n < 3)
            dir = 0;
        if (gr() < 0.5f)
            kp_add(0, PK_CHORD, 0, 16, 44, 2);     /* a soft bed under it */
        for (s = 0, i = 0; s < 16; s += rate, i++) {
            int idx, note;
            if (dir == 0)
                idx = i % n;                         /* up */
            else if (dir == 1)
                idx = n - 1 - i % n;                 /* down */
            else if (dir == 2)
                idx = (i / (n - 1)) & 1 ? n - 1 - i % (n - 1) : i % (n - 1);   /* up and down */
            else
                idx = gri(n);                        /* scattered */
            if (idx < 0 || idx >= n)
                idx = 0;
            note = seq[idx];
            if (rate == 1 && (s & 1) && gr() < 0.4f)
                continue;
            kp_add(s, PK_NOTE, note, rate + 1, (s & 3) == 0 ? 84 : 66 + gri(10), 0);
        }
        break;
    }
    case KST_SPARSE: {
        int k = 1 + d10 / 3, used = 0;
        static const uint8_t AT[7] = {3, 6, 7, 10, 11, 13, 14};
        kp_add(0, PK_CHORD, 0, 16, 70, 3);
        for (i = 0; i < k; i++) {
            int st = AT[gri(7)], note;
            if (used >> st & 1)
                continue;
            used |= 1 << st;
            note = scale_near(top + 2 + gri(9), gr() < 0.5f ? 1 : -1);
            kp_add(st, PK_NOTE, note, 2 + gri(3), 58 + gri(18), 0);
        }
        break;
    }
    default:
        break;
    }
    /* a little melodic answer at the end of every other bar */
    if (kstyle != KST_SPARSE && kstyle != KST_ARP && (bar & 1u) && gr() < 0.12f + var_now * 0.4f) {
        int n = scale_near(top + 3 + gri(4), 1), st = 12 + gri(2);
        for (i = 0; i < 3 && st < 16; i++) {
            kp_add(st, PK_NOTE, n, 2, 62 + gri(14), 0);
            n = scale_near(n - 1 - gri(2), -1);
            st += 1 + gri(2);
        }
    }
}

static void plan_bass(int bstyle, int next_root_pc)
{
    int r = bass_of(chord_root_pc), f = r + 7, o = r + 12, nr = bass_of(next_root_pc), a, i;
    const quality_t *q = &QUALS[chord_q];
    int third = r + q->iv[1] % 12, seventh = r + 10;
    for (i = 0; i < q->n; i++)
        if (q->iv[i] == 11 || q->iv[i] == 10 || q->iv[i] == 9)
            seventh = r + q->iv[i];
    if (nr - r > 6)
        nr -= 12;
    if (r - nr > 6)
        nr += 12;
    a = nr == r ? r + (gr() < 0.5f ? 7 : -2) : nr + (nr > r ? -1 : 1) * (gr() < 0.6f ? 1 : 2);
    if (a < 26)
        a += 12;
    nbplan = 0;
    switch (bstyle) {
    case BST_HOLD:
        bp_add(0, r, gr() < density_now ? 10 : 15, 100);
        if (gr() < density_now * 0.6f)
            bp_add(gr() < 0.5f ? 10 : 11, gr() < 0.5f ? o : f, 3, 80);
        if (gr() < 0.25f + density_now * 0.4f)
            bp_add(14, a, 2, 84);
        break;
    case BST_BOUNCE: {
        int t = gri(3);
        if (t == 0) {
            bp_add(0, r, 3, 104);
            bp_add(6, gr() < 0.5f ? o : r, 2, 80);
            bp_add(10, r, 2, 88);
            bp_add(14, gr() < 0.5f ? f : a, 2, 82);
        } else if (t == 1) {
            bp_add(0, r, 5, 104);
            bp_add(6, r, 2, 76);
            bp_add(8, f, 3, 88);
            if (density_now > 0.4f)
                bp_add(11, r, 1, 70);
            bp_add(14, a, 2, 84);
        } else {
            bp_add(0, r, 6, 104);
            bp_add(7, r, 1, 72);
            bp_add(10, gr() < 0.5f ? o : seventh, 2, 84);
            bp_add(12, f, 3, 86);
            if (density_now > 0.5f)
                bp_add(15, a, 1, 78);
        }
        break;
    }
    case BST_WALK: {
        int x = gr() < 0.5f ? third : scale_near(r + 2, 1), y = gr() < 0.6f ? f : seventh;
        bp_add(0, r, 3, 100);
        bp_add(4, x, 3, 84);
        bp_add(8, y, 3, 88);
        if (density_now > 0.6f && gr() < 0.5f) {
            bp_add(12, scale_near(a + 1, 1), 1, 76);
            bp_add(14, a, 2, 82);
        } else {
            bp_add(12, a, 3, 84);
        }
        break;
    }
    default:
        break;
    }
}

static float drum_v(char c) { return c == 'X' ? 1.0f : c == 'x' ? 0.72f : c == 'o' ? 0.36f : 0.0f; }

static void plan_drums(void)
{
    const pattern_t *p = &PATTERNS[cur_pat];
    const char *row[NDR] = {p->k, p->s, p->h, p->o, p->r, p->p};
    int d, s;
    for (d = 0; d < NDR; d++)
        for (s = 0; s < 16; s++) {
            float v = drum_v(row[d][s]);
            if (v > 0.0f)
                v *= 0.9f + 0.2f * gr() - (d == DR_HAT ? 0.12f * (float)(s & 1) : 0.0f);
            dplan[d][s] = (uint8_t)(fm_clampf(v, 0.0f, 1.0f) * 127.0f);
        }
    /* ghosts, by density and variation: hats on the empty sixteenths, a snare whisper */
    for (s = 1; s < 16; s += 2)
        if (!dplan[DR_HAT][s] && !dplan[DR_OHAT][s] && p->h[s - 1] != '.' && gr() < density_now * 0.35f * (0.4f + var_now))
            dplan[DR_HAT][s] = (uint8_t)(30 + gri(20));
    if (p->s[4] != '.' && gr() < (0.15f + density_now * 0.3f) * (0.3f + var_now)) {
        static const uint8_t G[4] = {7, 9, 15, 3};
        s = G[gri(4)];
        if (!dplan[DR_SNARE][s])
            dplan[DR_SNARE][s] = (uint8_t)(28 + gri(18));
    }
    /* a fill at the end of every fourth bar */
    if ((bar & 3u) == 3u && gr() < 0.25f + var_now * 0.5f) {
        int t = gri(3);
        eng_stat.xfills++;
        if (t == 0) {                   /* a little snare run up */
            for (s = 12; s < 16; s++)
                dplan[DR_SNARE][s] = (uint8_t)(50 + (s - 12) * 18);
        } else if (t == 1) {            /* kick and snare answer */
            dplan[DR_KICK][12] = 110;
            dplan[DR_SNARE][14] = 115;
            dplan[DR_SNARE][15] = 70;
            dplan[DR_HAT][14] = 0;
        } else {                        /* the hats drop out, an open hat to land */
            for (s = 8; s < 16; s++)
                dplan[DR_HAT][s] = 0;
            dplan[DR_OHAT][14] = 100;
        }
    }
}

static void bar_begin(void)
{
    const mood_t *m;
    int ks_style, bs_style, next;
    uint32_t loop;
    /* the changes asked for land here */
    cur_mood = (int)clamp(eng_ctl.mood, 0, NMOOD - 1);
    cur_root = (int)clamp(eng_ctl.root, 0, 11);
    cur_pat = (int)clamp(eng_ctl.pattern, 0, NPATTERN - 1);
    m = &MOODS[cur_mood];
    if (cur_ksound != m->ksound) {
        cur_ksound = m->ksound;
        ksound_set(cur_ksound);
    }
    kit_set(PATTERNS[cur_pat].kit);
    ks_style = (int)eng_ctl.kstyle;
    bs_style = (int)eng_ctl.bstyle;
    cur_kstyle = ks_style <= KST_AUTO || ks_style >= NKSTYLE ? m->kstyle : ks_style;
    cur_bstyle = bs_style <= BST_AUTO || bs_style >= NBSTYLE ? m->bstyle : bs_style;
    /* the loop's own random for this bar */
    loop = m->n < 8 ? (uint32_t)m->n * 2u : (uint32_t)m->n;
    rn_det = 0x9E3779B9u ^ ((uint32_t)cur_mood * 2654435761u) ^ ((uint32_t)cur_pat * 40503u) ^ ((bar % loop) * 97003u);
    if (!rn_det)
        rn_det = 1u;
    xs(&rn_det);
    xs(&rn_det);
    eng_stat.chord = (int32_t)(bar % m->n);
    chord_root_pc = pc_of(cur_root + m->prog[bar % m->n].deg);
    chord_q = m->prog[bar % m->n].q;
    next = pc_of(cur_root + m->prog[(bar + 1u) % m->n].deg);
    voice_chord();
    {
        const quality_t *q = &QUALS[chord_q];
        uint32_t mask = 0;
        int i;
        for (i = 0; i < q->n; i++)
            mask |= 1u << pc_of(chord_root_pc + q->iv[i]);
        eng_stat.keys_mask = mask;
    }
    plan_keys(cur_kstyle);
    plan_bass(cur_bstyle, next);
    plan_drums();
    eng_stat.chord_root = chord_root_pc;
    eng_stat.mood = cur_mood;
    eng_stat.root = cur_root;
    eng_stat.pattern = cur_pat;
}

static float hum(float ms) { return (gr() - 0.5f) * ms * (SR / 1000.0f); }

static void do_step(void)
{
    int i, d;
    float lazy;
    uint32_t strum_t;
    step = (step + 1) & 15;
    if (step == 0) {
        bar++;
        bar_begin();
    }
    eng_stat.step = step;
    eng_stat.bar = bar;
    if ((step & 3) == 0)
        eng_stat.beats++;
    /* drums */
    if (!eng_ctl.breakdown) {
        lazy = (float)PATTERNS[cur_pat].lazy_ms * (SR / 1000.0f);
        for (d = 0; d < NDR; d++) {
            float t;
            int v = dplan[d][step];
            if (!v)
                continue;
            t = 60.0f + hum(3.0f) + ((d == DR_SNARE || d == DR_RIM) ? lazy : 0.0f);
            if (d == DR_KICK)
                t = 60.0f;
            ev_add((uint32_t)(t < 0.0f ? 0.0f : t), EV_DRUM, d, v, 0);
        }
    }
    /* piano */
    for (i = 0; i < nkplan; i++) {
        kplan_t *k = &kplan[i];
        uint32_t len_s;
        if (k->step != step)
            continue;
        len_s = (uint32_t)((float)k->len * sps) - 200u;
        if (k->kind == PK_CHORD) {
            int j;
            float per = (float)k->strum * 0.008f * SR;   /* the roll: 8 ms a unit, from the bottom */
            strum_t = (uint32_t)(80.0f + (hum(4.0f) > 0.0f ? hum(4.0f) : 0.0f));
            for (j = 0; j < nvoice; j++) {
                uint16_t tag = ++tag_n;
                uint32_t at = strum_t + (uint32_t)(per * (float)j);
                int v = k->vel - (j == nvoice - 1 ? 0 : 6) + (int)(hum(1.0f) * 0.1f);
                ev_add(at, EV_KON, voicing[j], clamp(v, 1, 127), tag);
                ev_add(at + len_s, EV_KOFF, voicing[j], 0, tag);
            }
        } else {
            uint16_t tag = ++tag_n;
            float t = 80.0f + hum(5.0f);
            uint32_t at = (uint32_t)(t < 0.0f ? 0.0f : t);
            ev_add(at, EV_KON, k->note, k->vel, tag);
            ev_add(at + len_s, EV_KOFF, k->note, 0, tag);
        }
    }
    /* bass */
    if (lvl_bass > 0.0f || eng_ctl.midi_out)
        for (i = 0; i < nbplan; i++) {
            bplan_t *b = &bplan[i];
            if (b->step == step) {
                uint16_t tag = ++tag_n;
                float t = 70.0f + hum(3.0f);
                uint32_t at = (uint32_t)(t < 0.0f ? 0.0f : t);
                ev_add(at, EV_BON, b->note, b->vel, tag);
                ev_add(at + (uint32_t)((float)b->len * sps) - 400u, EV_BOFF, b->note, 0, tag);
            }
        }
}

static float step_dur(int st)
{
    float sw = swing_now * 0.333f;
    return sps * ((st & 1) ? 1.0f - sw : 1.0f + sw);
}

static void all_notes_off(void)
{
    int i;
    for (i = 0; i < NEV; i++)
        evq[i].type = EV_NONE;
    next_due = 0xFFFFFFFFu;
    for (i = 0; i < NKV; i++)
        if (kv[i].stage && kv[i].stage < 3)
            kv[i].stage = 3;
    bs.on = 0;
    eng_stat.bass_note = -1;
    if (eng_ctl.midi_out) {
        midi3(0xB0u, 123u, 0u);
        midi3(0xB1u, 123u, 0u);
    }
}

/* ------------------------------------------------------- the outside --- */
static struct {
    float l1, l2, r1, r2, lfo, lfo2;
    float drop, drop_ph, drop_inc, drop_pan;
    float cr_t[2], cr_ph[2];
    float wave_ph;
    float hum_ph1, hum_ph2;
} amb;

static inline void ambience(int type, float lvl, float *l, float *r)
{
    float nl = noise(), nr = noise(), a, b;
    amb.lfo = wrap1(amb.lfo + 0.07f / SR);
    amb.lfo2 = wrap1(amb.lfo2 + 0.013f / SR);
    switch (type) {
    case AMB_RAIN: {
        /* a hush of noise, and drops */
        amb.l1 += (nl - amb.l1) * 0.30f;
        amb.l2 += (amb.l1 - amb.l2) * 0.05f;
        amb.r1 += (nr - amb.r1) * 0.30f;
        amb.r2 += (amb.r1 - amb.r2) * 0.05f;
        a = (0.75f + 0.25f * sin_t(amb.lfo)) * 0.55f;
        *l = (amb.l1 - amb.l2) * a;
        *r = (amb.r1 - amb.r2) * a;
        if (amb.drop < 0.001f && frand(&rn_noise) < 0.0009f) {
            amb.drop = 0.10f + 0.25f * frand(&rn_noise);
            amb.drop_inc = (1800.0f + 2600.0f * frand(&rn_noise)) / SR;
            amb.drop_pan = frand(&rn_noise);
            amb.drop_ph = 0.0f;
        }
        if (amb.drop > 0.001f) {
            float s = sin_t(amb.drop_ph) * amb.drop;
            amb.drop_ph = wrap1(amb.drop_ph + amb.drop_inc);
            amb.drop_inc *= 0.99985f;
            amb.drop *= 0.9975f;
            *l += s * (1.0f - amb.drop_pan);
            *r += s * amb.drop_pan;
        }
        break;
    }
    case AMB_CRICKETS: {
        int c;
        amb.l1 += (nl - amb.l1) * 0.02f;
        *l = *r = amb.l1 * 0.25f;
        for (c = 0; c < 2; c++) {          /* two crickets: three pulses, then a rest */
            float t = amb.cr_t[c], g = 0.0f;
            amb.cr_t[c] += 1.0f / SR;
            if (t < 0.12f) {
                float in = t - (float)(int32_t)(t * 25.0f) * 0.04f;   /* 40 ms apart */
                g = in < 0.018f ? sin_t(in * 27.0f) : 0.0f;
                if (g < 0.0f)
                    g = 0.0f;
            } else if (t > (c ? 0.83f : 1.07f)) {
                amb.cr_t[c] = 0.0f;
            }
            amb.cr_ph[c] = wrap1(amb.cr_ph[c] + (c ? 4380.0f : 4720.0f) / SR);
            a = sin_t(amb.cr_ph[c]) * g * 0.06f;
            *l += c ? a * 0.4f : a;
            *r += c ? a : a * 0.4f;
        }
        break;
    }
    case AMB_WIND: {
        float k = 0.004f + 0.012f * (0.5f + 0.5f * sin_t(amb.lfo2 * 3.0f));
        amb.l1 += (nl - amb.l1) * k;
        amb.l2 += (amb.l1 - amb.l2) * k;
        amb.r1 += (nr - amb.r1) * k;
        amb.r2 += (amb.r1 - amb.r2) * k;
        a = (0.5f + 0.5f * sin_t(amb.lfo)) * 2.2f;
        *l = amb.l2 * a;
        *r = amb.r2 * a;
        break;
    }
    case AMB_WAVES: {
        float sw;
        amb.wave_ph = wrap1(amb.wave_ph + 0.11f / SR);
        sw = amb.wave_ph < 0.35f ? amb.wave_ph / 0.35f : (1.0f - amb.wave_ph) / 0.65f;
        sw = sw * sw;
        a = 0.01f + 0.05f * sw;
        amb.l1 += (nl - amb.l1) * a;
        amb.r1 += (nr - amb.r1) * a;
        b = 0.25f + 0.9f * sw;
        *l = amb.l1 * b;
        *r = amb.r1 * b;
        break;
    }
    default: {                           /* the hum of a ship in orbit */
        amb.hum_ph1 = wrap1(amb.hum_ph1 + 55.0f / SR);
        amb.hum_ph2 = wrap1(amb.hum_ph2 + 82.6f / SR);
        amb.l1 += (nl - amb.l1) * 0.01f;
        a = sin_t(amb.hum_ph1) * 0.12f + sin_t(amb.hum_ph2) * 0.06f * (0.6f + 0.4f * sin_t(amb.lfo));
        *l = a + amb.l1 * 0.4f;
        *r = a - amb.l1 * 0.4f;
        break;
    }
    }
    *l *= lvl;
    *r *= lvl;
}

/* ----------------------------------------------------------- render --- */
static float out_l1, out_l2, out_r1, out_r2, tone_k, hp_l, hp_r, hpx_l, hpx_r;
static float cr_amp, cr_lp, hiss_lp, pop;
static float trem_ph, level_s;

void eng_init(void)
{
    int i;
    for (i = 0; i <= SINE_N; i++)
        sine_t[i] = fm_sinf((float)i * (FM_TWO_PI / (float)SINE_N));
    cmb[0] = (comb_t){cb_l0, C0, 0, 0.0f};
    cmb[1] = (comb_t){cb_l1, C1, 0, 0.0f};
    cmb[2] = (comb_t){cb_l2, C2, 0, 0.0f};
    cmb[3] = (comb_t){cb_l3, C3, 0, 0.0f};
    cmb[4] = (comb_t){cb_r0, C0 + RS, 0, 0.0f};
    cmb[5] = (comb_t){cb_r1, C1 + RS, 0, 0.0f};
    cmb[6] = (comb_t){cb_r2, C2 + RS, 0, 0.0f};
    cmb[7] = (comb_t){cb_r3, C3 + RS, 0, 0.0f};
    aps[0] = (ap_t){ap_l0, A0, 0};
    aps[1] = (ap_t){ap_l1, A1, 0};
    aps[2] = (ap_t){ap_r0, A0 + RS, 0};
    aps[3] = (ap_t){ap_r1, A1 + RS, 0};
    ksound_set(KS_RHODES);
    kit_set(KIT_DUSTY);
    bs.katk = 1.0f - fm_expf(-1.0f / (0.004f * SR));
    bs.krel = tau_k(0.025f);
    bs.lpk = lp_k(700.0f);
    eng_stat.step = -1;
    eng_stat.chord_root = -1;
    eng_stat.bass_note = -1;
    eng_stat.top_note = -1;
}

void eng_panic(void)
{
    int i;
    for (i = 0; i < NKV; i++)
        kv[i].stage = 0;
    for (i = 0; i < NEV; i++)
        evq[i].type = EV_NONE;
    bs.on = 0;
    bs.amp = 0.0f;
    dr.ka = dr.sa = dr.sn = dr.ha = dr.ra = dr.pa = dr.p_atk = 0.0f;
}

void eng_blip(int up) { (void)up; }

void eng_render(int32_t *lr, uint32_t n, float gain)
{
    uint32_t i;
    int want = eng_ctl.playing != 0, amb_type = (int)eng_ctl.amb;
    float clk_dur, rev = eng_ctl.reverb, wow = eng_ctl.wow, crackle = eng_ctl.crackle, amb_lvl, duck_k;
    float fb = 0.78f + 0.12f * rev, damp = 0.35f, rev_in = 0.18f + 0.55f * rev, tone_hz;
    float trem_inc, kd = eng_ctl.drums, kb = eng_ctl.bass, kk = eng_ctl.keys;
    /* the block's settings */
    bpm_now = fm_clampf(eng_ctl.bpm, 50.0f, 160.0f);
    sps = SR * 60.0f / bpm_now * 0.25f;
    clk_dur = SR * 60.0f / bpm_now / 24.0f;
    swing_now = fm_maxf(fm_clampf(eng_ctl.swing, 0.0f, 1.0f), (float)PATTERNS[cur_pat].swing_min * 0.01f);
    var_now = fm_clampf(eng_ctl.variation, 0.0f, 1.0f);
    density_now = fm_clampf(eng_ctl.density, 0.0f, 1.0f);
    lvl_drums += (kd * kd * 1.1f - lvl_drums) * 0.2f;
    lvl_bass += (kb * kb * 1.0f - lvl_bass) * 0.2f;
    lvl_keys += (kk * kk * 1.0f - lvl_keys) * 0.2f;
    amb_lvl = eng_ctl.ambience;
    amb_lvl = amb_lvl * amb_lvl * 0.5f;
    tone_hz = 1100.0f * fm_exp2f(eng_ctl.tone * 3.9f);   /* 1.1 .. 16 kHz */
    tone_k = lp_k(tone_hz);
    duck_k = tau_k(0.16f);
    trem_inc = ks->trem_hz / SR;
    if (amb_type < 0 || amb_type >= NAMB)
        amb_type = 0;
    /* PLAY / STOP */
    if (want && !playing) {
        playing = 1;
        step = -1;
        bar = 0xFFFFFFFFu;
        step_left = 0.02f * SR;
        clk_left = step_left;
        midi_rt(0xFAu);
    } else if (!want && playing) {
        playing = 0;
        all_notes_off();
        eng_stat.step = -1;
        midi_rt(0xFCu);
    }
    for (i = 0; i < n; i++) {
        float kl = 0.0f, kr = 0.0f, b, dl, dr_, ml, mr, rl, rr, al, ar, l, r, wd, wr, fr, x;
        int v;
        int32_t wi;
        if (playing) {
            step_left -= 1.0f;
            if (step_left <= 0.0f) {
                do_step();
                step_left += step_dur(step);
            }
            clk_left -= 1.0f;
            if (clk_left <= 0.0f) {
                clk_left += clk_dur;
                midi_rt(0xF8u);
            }
        }
        if (next_due != 0xFFFFFFFFu && (int32_t)(next_due - t_now) <= 0)
            run_events();
        t_now++;
        /* the band */
        for (v = 0; v < NKV; v++)
            if (kv[v].stage)
                keys_voice(&kv[v], &kl, &kr);
        trem_ph = wrap1(trem_ph + trem_inc);
        if (ks->trem > 0.0f) {
            float t = sin_t(trem_ph) * ks->trem;
            if (ks->trem_pan) {
                kl *= 1.0f - t;
                kr *= 1.0f + t;
            } else {
                kl *= 1.0f - 0.5f * ks->trem + 0.5f * t;
                kr *= 1.0f - 0.5f * ks->trem + 0.5f * t;
            }
        }
        kl *= lvl_keys * 1.8f;
        kr *= lvl_keys * 1.8f;
        b = bass_sample() * lvl_bass * 0.5f;
        drums_sample(&dl, &dr_);
        /* the drums through an old sampler: held, fewer bits, darker */
        if (++dr.hold_n >= kit->hold) {
            float q = fm_exp2f(kit->bits - 1.0f);
            dr.hold_n = 0;
            dr.hold_l = (float)(int32_t)(dl * q) / q;
            dr.hold_r = (float)(int32_t)(dr_ * q) / q;
        }
        dr.bus_l += (dr.hold_l - dr.bus_l) * dr.bus_k;
        dr.bus_r += (dr.hold_r - dr.bus_r) * dr.bus_k;
        dl = dr.bus_l * lvl_drums * 0.55f;
        dr_ = dr.bus_r * lvl_drums * 0.55f;
        /* sidechain: the kick pushes the keys and bass down a little */
        duck *= duck_k;
        ml = (kl + b) * (1.0f - 0.32f * duck);
        mr = (kr + b) * (1.0f - 0.32f * duck);
        /* wow and flutter: the keys and bass through a wobbling tape delay */
        wow_l[wow_w] = ml;
        wow_r[wow_w] = mr;
        wow_ph = wrap1(wow_ph + 0.55f / SR);
        flut_ph = wrap1(flut_ph + 6.3f / SR);
        if ((t_now & 4095u) == 0u)
            wow_drift_to = frand(&rn_free) - 0.5f;
        wow_drift += (wow_drift_to - wow_drift) * 0.00005f;
        wd = 0.004f * SR + wow * (0.0032f * SR * (sin_t(wow_ph) + 0.6f * wow_drift) + 0.0003f * SR * sin_t(flut_ph));
        wr = (float)wow_w - wd;
        if (wr < 0.0f)
            wr += (float)WOW_N;
        wi = (int32_t)wr;
        fr = wr - (float)wi;
        {
            int32_t w2 = (wi + 1) & (WOW_N - 1);
            ml = wow_l[wi] + (wow_l[w2] - wow_l[wi]) * fr;
            mr = wow_r[wi] + (wow_r[w2] - wow_r[wi]) * fr;
        }
        wow_w = (wow_w + 1u) & (WOW_N - 1);
        /* reverb on the keys (and a little of the snare) */
        x = ((ml + mr) * 0.5f - b * 0.8f + (dl + dr_) * 0.12f) * rev_in * 0.5f + 1e-12f;   /* (no denormals) */
        rl = comb(&cmb[0], x, fb, damp) + comb(&cmb[1], x, fb, damp) + comb(&cmb[2], x, fb, damp) +
             comb(&cmb[3], x, fb, damp);
        rr = comb(&cmb[4], x, fb, damp) + comb(&cmb[5], x, fb, damp) + comb(&cmb[6], x, fb, damp) +
             comb(&cmb[7], x, fb, damp);
        rl = allp(&aps[1], allp(&aps[0], rl));
        rr = allp(&aps[3], allp(&aps[2], rr));
        l = ml + dl + rl * rev * 0.45f;
        r = mr + dr_ + rr * rev * 0.45f;
        /* the outside */
        if (amb_lvl > 0.0001f) {
            ambience(amb_type, amb_lvl, &al, &ar);
            l += al;
            r += ar;
        }
        /* vinyl: crackle, pops and hiss */
        if (crackle > 0.001f) {
            float c;
            if (frand(&rn_noise) < crackle * 0.0006f)
                cr_amp = (frand(&rn_noise) - 0.5f) * (0.25f + 0.5f * crackle);
            if (frand(&rn_noise) < crackle * 0.000012f)
                pop = (frand(&rn_noise) < 0.5f ? -1.0f : 1.0f) * 0.35f * crackle;
            c = cr_amp;
            cr_amp *= 0.55f;
            cr_lp += (pop - cr_lp) * 0.08f;
            pop *= 0.985f;
            hiss_lp += (noise() - hiss_lp) * 0.25f;
            c += cr_lp + hiss_lp * 0.010f * crackle;
            l += c;
            r += c * 0.9f;
        }
        /* the tape's tone: a warm two-pole low-pass, the lows kept clean, a gentle saturation */
        out_l1 += (l - out_l1) * tone_k;
        out_l2 += (out_l1 - out_l2) * tone_k;
        out_r1 += (r - out_r1) * tone_k;
        out_r2 += (out_r1 - out_r2) * tone_k;
        hp_l = 0.9965f * (hp_l + out_l2 - hpx_l);
        hpx_l = out_l2;
        hp_r = 0.9965f * (hp_r + out_r2 - hpx_r);
        hpx_r = out_r2;
        l = softclip(hp_l * 1.15f) * 0.85f;
        r = softclip(hp_r * 1.15f) * 0.85f;
        level_s += (fm_fabsf(l) - level_s) * 0.0005f;
        lr[2 * i] = (int32_t)(l * gain);
        lr[2 * i + 1] = (int32_t)(r * gain);
    }
    for (i = 0; i < NKV; i++) {            /* (denormals: the voices' filters) */
        kv[i].lp = fm_flush(kv[i].lp);
    }
    dr.bus_l = fm_flush(dr.bus_l);
    dr.bus_r = fm_flush(dr.bus_r);
    dr.sn_lp = fm_flush(dr.sn_lp);
    dr.sn_hp = fm_flush(dr.sn_hp);
    amb.l1 = fm_flush(amb.l1);
    amb.l2 = fm_flush(amb.l2);
    amb.r1 = fm_flush(amb.r1);
    amb.r2 = fm_flush(amb.r2);
    cr_lp = fm_flush(cr_lp);
    hp_l = fm_flush(hp_l);
    hp_r = fm_flush(hp_r);
    out_l1 = fm_flush(out_l1);
    out_l2 = fm_flush(out_l2);
    out_r1 = fm_flush(out_r1);
    out_r2 = fm_flush(out_r2);
    eng_stat.level = level_s * 3.0f;
    eng_stat.duck = duck;
}
