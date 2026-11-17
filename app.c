/* SPDX-License-Identifier: MIT
 * NOOK: lo-fi jam companion for the M-VAVE FM-1 (on fm1-base). */
/* The app: the controls, the settings, and the screen, a cosy room with a window onto one of eight scenes, a cat
 * on the sill who keeps time, a cassette deck, and what the guitarist needs to know: the mood, the chord now, the
 * key and scale, the drum pattern, the tempo. Part of the unity build (device/unity.c, host/plat_host.c), after the
 * platform: gfx.c and announce.c are in scope. The band itself is dsp/lofi.c. */
#include "fastmath.h"
#include "lofi.h"
#include "music.h"

/* =================================================================== settings === */
#define SET_MAGIC 0x4E4F4B32u            /* "NOK2" */
enum { PG_MAIN, PG_GROOVE, PG_TAPE, NPAGE };
typedef struct {
    uint32_t magic;
    uint8_t mood, pattern, theme, root;
    uint8_t kstyle, bstyle, page, cat;
    uint8_t bpm, swing, variation, density;
    uint8_t drums, bass, keys, amb;
    uint8_t crackle, wow, tone, reverb;
    uint8_t clean, rsv[3];
} settings_t;
static settings_t set;
static uint8_t set_dirty;
static uint32_t dirty_at;
static uint8_t playing, breakdown;

static void settings_defaults(void)
{
    set.magic = SET_MAGIC;
    set.mood = 0;
    set.pattern = 0;
    set.theme = TH_AUTO;
    set.root = MOODS[0].root;
    set.kstyle = KST_AUTO;
    set.bstyle = BST_AUTO;
    set.page = PG_MAIN;
    set.cat = 0;
    set.bpm = MOODS[0].bpm;
    set.swing = 45;
    set.variation = 35;
    set.density = 50;
    set.drums = 80;
    set.bass = 75;
    set.keys = 80;
    set.amb = 30;
    set.crackle = 35;
    set.wow = 40;
    set.tone = 62;
    set.reverb = 40;
    set.clean = 0;
}

static int settings_ok(void)
{
    return set.magic == SET_MAGIC && set.mood < NMOOD && set.pattern < NPATTERN && set.theme < NTHEME &&
           set.root < 12 && set.kstyle < NKSTYLE && set.bstyle < NBSTYLE && set.page < NPAGE && set.cat < 4 &&
           set.bpm >= 50 && set.bpm <= 160 && set.swing <= 100 && set.variation <= 100 && set.density <= 100 &&
           set.drums <= 100 && set.bass <= 100 && set.keys <= 100 && set.amb <= 100 && set.crackle <= 100 &&
           set.wow <= 100 && set.tone <= 100 && set.reverb <= 100 && set.clean <= 1;
}

#define KEYNAME(pc) note_in_key((pc), set.root, MOODS[set.mood].scale)
static int theme_now(void) { return set.theme == TH_AUTO ? MOODS[set.mood].theme : set.theme; }

/* everything to the band (it takes a new mood, key or pattern at the next bar line) */
static void send_all(void)
{
    eng_ctl.mood = set.mood;
    eng_ctl.root = set.root;
    eng_ctl.pattern = set.pattern;
    eng_ctl.kstyle = set.kstyle;
    eng_ctl.bstyle = set.bstyle;
    eng_ctl.breakdown = breakdown;
    eng_ctl.amb = THEME_AMB[theme_now()];
    eng_ctl.midi_out = 1;
    eng_ctl.bpm = (float)set.bpm;
    eng_ctl.swing = (float)set.swing * 0.01f;
    eng_ctl.variation = (float)set.variation * 0.01f;
    eng_ctl.density = (float)set.density * 0.01f;
    eng_ctl.drums = (float)set.drums * 0.01f;
    eng_ctl.bass = (float)set.bass * 0.01f;
    eng_ctl.keys = (float)set.keys * 0.01f;
    eng_ctl.ambience = (float)set.amb * 0.01f;
    eng_ctl.crackle = (float)set.crackle * 0.01f;
    eng_ctl.wow = (float)set.wow * 0.01f;
    eng_ctl.tone = (float)set.tone * 0.01f;
    eng_ctl.reverb = (float)set.reverb * 0.01f;
    eng_ctl.playing = playing;
}

void app_init(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || !settings_ok())
        settings_defaults();
    eng_init();
    send_all();
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    eng_render(lr, n, (float)master_q12 * (1.0f / 4096.0f) * 8300000.0f);
}

void app_panic(void) { eng_panic(); }

/* ======================================================================= text === */
static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}
static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}
static void two_digits(int v, char *b)
{
    b[0] = (char)('0' + v / 10 % 10);
    b[1] = (char)('0' + v % 10);
    b[2] = 0;
}

/* ===================================================================== the UI === */
enum { OV_NONE, OV_KNOBS, OV_BANNER, OV_HELP };
static struct {
    uint32_t frame_at, btn, keys;
    float t;                             /* seconds, for the animations */
    uint8_t overlay;
    uint32_t overlay_until;
    int8_t knob_hi;                      /* the knob just turned (-1 none) */
    char ban[3][28];                     /* a banner: small line, big line, small line */
    uint32_t home_t, tap_t[5];
    uint8_t ntap, scale_view, help;
    uint32_t enc_t[NE];
    char msg[2][24];
    uint32_t msg_until;
    /* what the band did last frame, to see what changed */
    uint32_t kicks, snares, beats, keyhits;
    int32_t step;
    uint32_t step_at;
} ui;

static void dirty(void)
{
    set_dirty = 1;
    dirty_at = plat_ms();
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[56];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1500u;
    if (ann_on()) {                      /* and to a screen reader (announce.c, fm1-reader) */
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void banner(const char *a, const char *b, const char *c)
{
    uint32_t i;
    const char *s[3] = {a, b, c};
    int k;
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 27u && s[k] && s[k][i]; i++)
            ui.ban[k][i] = s[k][i];
        ui.ban[k][i] = 0;
    }
    ui.overlay = OV_BANNER;
    ui.overlay_until = plat_ms() + 1800u;
    if (ann_on()) {
        char t[90];
        t[0] = 0;
        text_cat(t, a);
        text_cat(t, ". ");
        text_cat(t, b);
        if (c && c[0]) {
            text_cat(t, ". ");
            text_cat(t, c);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

/* ---------------------------------------------------------------- the knobs --- */
typedef struct {
    const char *label;
    uint8_t *v;
    uint8_t lo, hi, step;
} kdef_t;
static const char *const PAGE_NAME[NPAGE] = {"MIX", "GROOVE", "TAPE"};
static kdef_t KD[NPAGE][4];
static void knobs_init(void)
{
    kdef_t m[NPAGE][4] = {
        {{"TEMPO", &set.bpm, 50, 160, 1}, {"DRUMS", &set.drums, 0, 100, 2}, {"BASS", &set.bass, 0, 100, 2},
         {"KEYS", &set.keys, 0, 100, 2}},
        {{"SWING", &set.swing, 0, 100, 2}, {"VARIATION", &set.variation, 0, 100, 2},
         {"DENSITY", &set.density, 0, 100, 2}, {"OUTSIDE", &set.amb, 0, 100, 2}},
        {{"CRACKLE", &set.crackle, 0, 100, 2}, {"WOW", &set.wow, 0, 100, 2}, {"TONE", &set.tone, 0, 100, 2},
         {"REVERB", &set.reverb, 0, 100, 2}},
    };
    uint32_t i, j;
    for (i = 0; i < NPAGE; i++)
        for (j = 0; j < 4; j++)
            KD[i][j] = m[i][j];
}
static const char *knob_label(int p, int k)
{
    if (p == PG_GROOVE && k == 3)        /* the sound outside, by the window's */
        return AMB_NAME[THEME_AMB[theme_now()]];
    return KD[p][k].label;
}
static void knob_value_text(int p, int k, char *b)
{
    int v = *KD[p][k].v;
    int_text(v, b);
    if (p == PG_MAIN && k == 0)
        text_cat(b, " BPM");
    else
        text_cat(b, "%");
}

static void show_knobs(int hi)
{
    ui.overlay = OV_KNOBS;
    ui.knob_hi = (int8_t)hi;
    ui.overlay_until = plat_ms() + 2200u;
}

static void knob(int k, int32_t e)
{
    kdef_t *d = &KD[set.page][k];
    uint32_t now = plat_ms();
    int32_t st = d->step, v;
    char t[16];
    if (now - ui.enc_t[EN_K1 + k] < 70u)  /* turned fast: bigger steps */
        st *= 3;
    ui.enc_t[EN_K1 + k] = now;
    v = clamp((int32_t)*d->v + e * st, d->lo, d->hi);
    if (v != *d->v) {
        *d->v = (uint8_t)v;
        dirty();
        send_all();
    }
    show_knobs(k);
    if (ann_on()) {
        knob_value_text(set.page, k, t);
        app_say(knob_label(set.page, k), t);
    }
}

/* ------------------------------------------------------------------- moods --- */
static void scale_line(char *b)          /* "A MINOR PENT" */
{
    b[0] = 0;
    text_cat(b, KEYNAME(set.root));
    text_cat(b, " ");
    text_cat(b, SCALES[MOODS[set.mood].scale].name);
}

static void mood_step(int32_t e)
{
    char a[20], c[28];
    int m = ((int)set.mood + (int)e % NMOOD + NMOOD) % NMOOD;
    set.mood = (uint8_t)m;
    set.root = MOODS[m].root;
    set.bpm = MOODS[m].bpm;
    dirty();
    send_all();
    a[0] = 0;
    text_cat(a, "MOOD ");
    two_digits(m + 1, a + 5);
    scale_line(c);
    banner(a, MOODS[m].name, c);
}

static void pattern_step(int32_t e)
{
    char a[20];
    static const char *const KIT_NAME[NKIT] = {"DUSTY KIT", "WARM KIT", "CRISP KIT", "SOFT KIT"};
    int p = ((int)set.pattern + (int)e % NPATTERN + NPATTERN) % NPATTERN;
    set.pattern = (uint8_t)p;
    dirty();
    send_all();
    a[0] = 0;
    text_cat(a, "DRUMS ");
    two_digits(p + 1, a + 6);
    banner(a, PATTERNS[p].name, KIT_NAME[PATTERNS[p].kit]);
}

static void theme_step(int32_t e)
{
    int t = ((int)set.theme + (int)e % NTHEME + NTHEME) % NTHEME;
    char c[28];
    set.theme = (uint8_t)t;
    dirty();
    send_all();
    c[0] = 0;
    if (t == TH_AUTO) {
        text_cat(c, "FOLLOWS THE MOOD");
        banner("WINDOW", "AUTO", c);
    } else {
        text_cat(c, "OUTSIDE: ");
        text_cat(c, AMB_NAME[THEME_AMB[t]]);
        banner("WINDOW", THEME_NAME[t], c);
    }
}

static void key_set(int pc)
{
    char c[28];
    pc = ((pc % 12) + 12) % 12;
    if (pc == set.root)
        return;
    set.root = (uint8_t)pc;
    dirty();
    send_all();
    scale_line(c);
    banner("KEY", KEYNAME(pc), c);
}

static void play_toggle(void)
{
    playing = (uint8_t)!playing;
    send_all();
    if (!playing)
        app_say("STOP", "");
    else
        app_say("PLAY", MOODS[set.mood].name);
}

static void tap_tempo(void)
{
    uint32_t now = plat_ms(), i, sum = 0;
    if (ui.ntap && now - ui.tap_t[ui.ntap - 1] > 2000u)
        ui.ntap = 0;
    if (ui.ntap == 5) {
        for (i = 0; i < 4; i++)
            ui.tap_t[i] = ui.tap_t[i + 1];
        ui.ntap = 4;
    }
    ui.tap_t[ui.ntap++] = now;
    if (ui.ntap >= 2) {
        int32_t bpm;
        for (i = 1; i < ui.ntap; i++)
            sum += ui.tap_t[i] - ui.tap_t[i - 1];
        bpm = (int32_t)(60000u * (ui.ntap - 1u) / (sum ? sum : 1u));
        while (bpm > 160)
            bpm /= 2;
        while (bpm < 50)
            bpm *= 2;
        set.bpm = (uint8_t)clamp(bpm, 50, 160);
        dirty();
        send_all();
        set.page = PG_MAIN;
        show_knobs(0);
    } else {
        app_say("TAP", "TEMPO");
    }
}

static void button(int b)
{
    char t[24];
    switch (b) {
    case B_PLAY:
        play_toggle();
        break;
    case B_FX:
    case B_ENV:
        set.page = (uint8_t)(set.page == (b == B_FX ? PG_TAPE : PG_GROOVE) ? PG_MAIN : b == B_FX ? PG_TAPE : PG_GROOVE);
        dirty();
        show_knobs(-1);
        if (ann_on())
            app_say("PAGE", PAGE_NAME[set.page]);
        break;
    case B_HOME:
        ui.home_t = plat_ms();
        if (set.page != PG_MAIN) {
            set.page = PG_MAIN;
            dirty();
        }
        show_knobs(-1);
        break;
    case B_ARP:
        set.kstyle = (uint8_t)((set.kstyle + 1) % NKSTYLE);
        dirty();
        send_all();
        t[0] = 0;
        if (set.kstyle == KST_AUTO) {
            text_cat(t, "AUTO: ");
            text_cat(t, KSTYLE_NAME[MOODS[set.mood].kstyle]);
        } else {
            text_cat(t, KSTYLE_NAME[set.kstyle]);
        }
        banner("KEYS PLAY", t, KSOUND_NAME[MOODS[set.mood].ksound]);
        break;
    case B_LFO:
        set.bstyle = (uint8_t)((set.bstyle + 1) % NBSTYLE);
        dirty();
        send_all();
        t[0] = 0;
        if (set.bstyle == BST_AUTO) {
            text_cat(t, "AUTO: ");
            text_cat(t, BSTYLE_NAME[MOODS[set.mood].bstyle]);
        } else {
            text_cat(t, BSTYLE_NAME[set.bstyle]);
        }
        banner("BASS PLAYS", t, "");
        break;
    case B_SEQ:
        breakdown = (uint8_t)!breakdown;
        send_all();
        banner("DRUMS", breakdown ? "SIT OUT" : "BACK IN", "FROM THE NEXT BAR");
        break;
    case B_REC:
        tap_tempo();
        break;
    case B_SAVE:
        if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
            set_dirty = 0;
            app_say("SAVED", "");
        } else {
            app_say("SAVE", "FAILED");
        }
        break;
    case B_SEL:
        ui.scale_view = (uint8_t)!ui.scale_view;
        ui.overlay = OV_NONE;
        if (ann_on()) {
            scale_line(t);
            app_say("SCALE", ui.scale_view ? t : "OFF");
        }
        break;
    case B_EDIT:
        set.clean = (uint8_t)!set.clean;
        dirty();
        app_say("VIEW", set.clean ? "WINDOW ONLY" : "INFO");
        break;
    case B_GLO: {
        static const char *const COAT[4] = {"GINGER", "MIDNIGHT", "SMOKE", "CALICO"};
        set.cat = (uint8_t)((set.cat + 1) % 4);
        dirty();
        banner("THE CAT", COAT[set.cat], "SAYS HI");
        break;
    }
    case B_OCTDN:
        key_set(set.root - 1);
        break;
    case B_OCTUP:
        key_set(set.root + 1);
        break;
    default:
        break;
    }
}

static uint32_t scale_keys, root_keys, chord_keys;
static void key_lights(void)
{
    const scale_t *s = &SCALES[MOODS[set.mood].scale];
    uint32_t k, sk = 0, rk = 0, ck = 0, cm = playing ? eng_stat.keys_mask : 0;
    for (k = 0; k < NKEYS; k++) {
        int pc = (int)((53u + k) % 12u), d = ((pc - set.root) % 12 + 12) % 12, i;
        for (i = 0; i < s->n; i++)
            if (s->iv[i] == d)
                sk |= 1u << k;
        if (d == 0)
            rk |= 1u << k;
        if (playing && eng_stat.chord_root == pc)
            ck |= 1u << k;
        (void)cm;
    }
    scale_keys = sk;
    root_keys = rk;
    chord_keys = ck;
}

static void lights(void)
{
    uint32_t lit = 0, dim = (1u << NB) - 1u, now = plat_ms();
    if (playing)
        lit |= LED_PLAY_GREEN;
    if (breakdown)
        lit |= 1u << B_SEQ;
    if (set.page == PG_TAPE)
        lit |= 1u << B_FX;
    if (set.page == PG_GROOVE)
        lit |= 1u << B_ENV;
    if (ui.scale_view)
        lit |= 1u << B_SEL;
    if (set.clean)
        lit |= 1u << B_EDIT;
    if (set_dirty)
        lit |= 1u << B_SAVE;
    if (set.kstyle != KST_AUTO)
        lit |= 1u << B_ARP;
    if (set.bstyle != BST_AUTO)
        lit |= 1u << B_LFO;
    if (playing && ui.step >= 0 && (ui.step & 3) == 0 && now - ui.step_at < 90u)   /* REC: the beat (tap along) */
        lit |= 1u << B_REC;
    key_lights();
    plat_lights(lit, root_keys | ui.keys, dim & ~lit, scale_keys, 1);
    plat_lights_mid(0, chord_keys & ~root_keys);
}

static void input(void)
{
    uint32_t btn = plat_buttons(), keys = plat_keys(), k, now = plat_ms();
    int32_t e;
    for (k = 0; k < NKEYS; k++)          /* a key: the key of the song (its note's name) */
        if ((keys >> k & 1u) && !(ui.keys >> k & 1u))
            key_set((int)((53u + k) % 12u));
    for (k = 0; k < NB; k++)
        if ((btn >> k & 1u) && !(ui.btn >> k & 1u))
            button((int)k);
    /* HOME held: the help */
    if ((btn >> B_HOME & 1u) && ui.home_t && now - ui.home_t > 600u) {
        if (!ui.help && ann_on())
            ann_say("HELP. SELECT MOOD. ALGORITHM DRUMS. PRESET WINDOW. KEYS SET THE KEY. PLAY STARTS.", 0, ANN_NOW);
        ui.help = 1;
    } else if (!(btn >> B_HOME & 1u)) {
        ui.help = 0;
        ui.home_t = 0;
    }
    if ((e = plat_enc(EN_SELECT)) != 0)
        mood_step(e > 0 ? 1 : -1);
    if ((e = plat_enc(EN_ALGO)) != 0)
        pattern_step(e > 0 ? 1 : -1);
    if ((e = plat_enc(EN_PRESET)) != 0)
        theme_step(e > 0 ? 1 : -1);
    for (k = 0; k < 4; k++)
        if ((e = plat_enc((int)(EN_K1 + k))) != 0)
            knob((int)k, e);
    ui.btn = btn;
    ui.keys = keys;
    /* saved by itself, a while after the last change */
    if (set_dirty && now - dirty_at > 4000u) {
        if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0)
            set_dirty = 0;
        else
            dirty_at = now;
    }
    lights();
}

void app_input_only(void) { input(); }

/* ================================================================ the picture === */
/* The room is drawn at 120 x 120 (each pixel 2 x 2 on the screen: pixel art), then the words over it at the
 * screen's own resolution. */
#define LW 120
#define LH 120
static uint16_t lofb[LW * LH] PLAT_POOL;
static int clx0, cly0, clx1 = LW, cly1 = LH;

#define C565(r, g, b) RGB(r, g, b)
static inline void clip(int x0, int y0, int x1, int y1)
{
    clx0 = x0 < 0 ? 0 : x0;
    cly0 = y0 < 0 ? 0 : y0;
    clx1 = x1 > LW ? LW : x1;
    cly1 = y1 > LH ? LH : y1;
}
static inline void noclip(void) { clip(0, 0, LW, LH); }
static inline void P(int x, int y, uint16_t c)
{
    if (x >= clx0 && x < clx1 && y >= cly0 && y < cly1)
        lofb[y * LW + x] = c;
}
static inline uint16_t G(int x, int y) { return (x >= 0 && x < LW && y >= 0 && y < LH) ? lofb[y * LW + x] : 0; }
static void R(int x, int y, int w, int h, uint16_t c)
{
    int i, j, x1 = x + w, y1 = y + h;
    if (x < clx0) x = clx0;
    if (y < cly0) y = cly0;
    if (x1 > clx1) x1 = clx1;
    if (y1 > cly1) y1 = cly1;
    for (j = y; j < y1; j++)
        for (i = x; i < x1; i++)
            lofb[j * LW + i] = c;
}
/* a+b blended, t of 16 toward b */
static inline uint16_t mix(uint16_t a, uint16_t b, int t)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    if (t <= 0) return a;
    if (t >= 16) return b;
    return (uint16_t)((((ar * (16 - t) + br * t) >> 4) << 11) | (((ag * (16 - t) + bg * t) >> 4) << 5) |
                      ((ab * (16 - t) + bb * t) >> 4));
}
static inline void PB(int x, int y, uint16_t c, int t)   /* a pixel blended over what is there */
{
    if (x >= clx0 && x < clx1 && y >= cly0 && y < cly1)
        lofb[y * LW + x] = mix(lofb[y * LW + x], c, t);
}
static void RB(int x, int y, int w, int h, uint16_t c, int t)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            PB(i, j, c, t);
}
static void HL(int x0, int x1, int y, uint16_t c)
{
    int x;
    for (x = x0; x <= x1; x++)
        P(x, y, c);
}
static void VL(int x, int y0, int y1, uint16_t c)
{
    int y;
    for (y = y0; y <= y1; y++)
        P(x, y, c);
}
static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1, err = dx + dy, g = 400;
    while (g--) {
        int e2 = 2 * err;
        P(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void ell(float cx, float cy, float rx, float ry, uint16_t c)
{
    int x, y, x0 = (int)(cx - rx - 1.0f), x1 = (int)(cx + rx + 1.0f), y0 = (int)(cy - ry - 1.0f), y1 = (int)(cy + ry + 1.0f);
    float irx = 1.0f / (rx * rx), iry = 1.0f / (ry * ry);
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            if (dx * dx * irx + dy * dy * iry <= 1.0f)
                P(x, y, c);
        }
}
static void disc(float cx, float cy, float r, uint16_t c) { ell(cx, cy, r, r, c); }
static void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)
{
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int x, y;
    for (y = miny; y <= maxy; y++)
        for (x = minx; x <= maxx; x++) {
            int w0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
            int w1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
            int w2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
                P(x, y, c);
        }
}

static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
/* a sky: c0 at y0 to c1 at y1 in a few bands, ordered dither between them (the pixel-art way) */
static void grad(int x0, int x1, int y0, int y1, uint16_t c0, uint16_t c1, int bands)
{
    int x, y;
    for (y = y0; y < y1; y++) {
        float t = (float)(y - y0) / (float)(y1 - y0 > 1 ? y1 - y0 - 1 : 1) * (float)(bands - 1);
        int b = (int)t, fr = (int)((t - (float)b) * 16.0f);
        uint16_t ca = mix(c0, c1, b * 16 / (bands - 1)), cb = mix(c0, c1, (b + 1) * 16 / (bands - 1));
        for (x = x0; x < x1; x++)
            P(x, y, BAYER[y & 3][x & 3] < fr ? cb : ca);
    }
}

static inline uint32_t hsh(uint32_t a)
{
    a ^= a >> 16;
    a *= 0x7FEB352Du;
    a ^= a >> 15;
    a *= 0x846CA68Bu;
    a ^= a >> 16;
    return a;
}
static inline float hf(uint32_t a) { return (float)(hsh(a) & 0xFFFFu) * (1.0f / 65536.0f); }
static inline float fwrap(float v, float m) { v -= (float)(int32_t)(v / m) * m; return v < 0.0f ? v + m : v; }
static inline float sn(float x) { return fm_sin_turns(x); }

/* ---------------------------------------------------------- the room --- */
typedef struct {
    uint16_t wall, wall2, frame, frame_dk, sill, curtain, curtain_dk, desk, desk_dk, panel, accent, accent2;
    uint8_t wallpat;                     /* 0 stripes, 1 planks, 2 dots, 3 metal */
    uint8_t night;
} room_t;
static const room_t ROOMS[NTHEME] = {
    /* AUTO (unused) */ {0},
    /* RAIN */   {C565(70, 48, 60), C565(78, 54, 66), C565(120, 82, 60), C565(80, 52, 40), C565(150, 104, 72),
                  C565(122, 70, 92), C565(92, 50, 72), C565(110, 72, 50), C565(76, 48, 36), C565(26, 20, 34),
                  C565(255, 196, 110), C565(140, 170, 230), 0, 1},
    /* SUNSET */ {C565(120, 76, 66), C565(132, 86, 74), C565(176, 120, 80), C565(124, 80, 52), C565(200, 146, 96),
                  C565(220, 140, 90), C565(180, 104, 70), C565(140, 92, 62), C565(100, 64, 44), C565(40, 24, 30),
                  C565(255, 190, 120), C565(255, 130, 110), 2, 0},
    /* SNOW */   {C565(110, 66, 44), C565(96, 56, 38), C565(150, 100, 64), C565(100, 64, 40), C565(176, 124, 80),
                  C565(170, 50, 50), C565(130, 34, 38), C565(124, 80, 52), C565(86, 54, 36), C565(30, 20, 18),
                  C565(255, 210, 140), C565(200, 220, 255), 1, 1},
    /* NEON */   {C565(48, 34, 70), C565(54, 38, 78), C565(90, 70, 110), C565(60, 44, 80), C565(110, 88, 130),
                  C565(70, 120, 140), C565(46, 86, 104), C565(70, 54, 90), C565(46, 34, 62), C565(16, 10, 26),
                  C565(255, 100, 200), C565(90, 230, 255), 0, 1},
    /* BEACH */  {C565(60, 76, 100), C565(66, 84, 110), C565(210, 210, 214), C565(150, 152, 160), C565(230, 228, 226),
                  C565(80, 140, 170), C565(56, 104, 132), C565(150, 120, 90), C565(110, 86, 64), C565(14, 20, 36),
                  C565(255, 230, 160), C565(140, 210, 255), 2, 1},
    /* AUTUMN */ {C565(168, 128, 92), C565(158, 118, 84), C565(110, 70, 46), C565(76, 46, 30), C565(140, 92, 60),
                  C565(110, 130, 70), C565(80, 100, 50), C565(120, 78, 50), C565(86, 54, 34), C565(40, 26, 20),
                  C565(255, 170, 70), C565(250, 220, 140), 1, 0},
    /* SAKURA */ {C565(232, 214, 200), C565(222, 202, 188), C565(140, 96, 80), C565(100, 66, 56), C565(170, 124, 104),
                  C565(240, 160, 180), C565(210, 126, 150), C565(160, 116, 92), C565(120, 84, 66), C565(50, 30, 40),
                  C565(255, 150, 190), C565(150, 190, 240), 2, 0},
    /* ORBIT */  {C565(62, 68, 80), C565(54, 60, 72), C565(130, 138, 150), C565(90, 96, 108), C565(150, 158, 170),
                  C565(80, 90, 104), C565(60, 68, 80), C565(84, 90, 102), C565(56, 60, 70), C565(10, 12, 20),
                  C565(120, 230, 200), C565(255, 160, 90), 3, 1},
};

/* the window: glass 19..100 x 19..74 */
#define GX0 19
#define GY0 19
#define GX1 101
#define GY1 75
#define GW (GX1 - GX0)
#define GH (GY1 - GY0)

static float beat_pos;                   /* beats since PLAY, with the fraction (the animations' clock) */
static float kick_pulse, snare_pulse;   /* 1 at a hit, decaying */

/* ---------------------------------------------------- the eight views --- */
static void rain_streaks(int n, uint16_t c, float speed, int len)
{
    int i;
    for (i = 0; i < n; i++) {
        float x = fwrap(hf(i * 7u + 1u) * 120.0f + ui.t * speed * 0.18f, (float)GW + 10.0f) - 5.0f;
        float y = fwrap(hf(i * 13u + 5u) * 80.0f + ui.t * speed, (float)GH + 8.0f) - 4.0f;
        int k;
        for (k = 0; k < len; k++)
            PB(GX0 + (int)x - k / 3, GY0 + (int)y - k, c, 10 - k * 2);
    }
}
static void glass_drops(uint16_t c)
{
    int i;
    for (i = 0; i < 7; i++) {
        float x = hf(i * 31u + 3u) * (float)GW;
        float sp = 1.0f + hf(i * 17u) * 3.0f;
        float y = fwrap(hf(i * 11u) * 60.0f + ui.t * sp, (float)GH + 6.0f) - 3.0f;
        PB(GX0 + (int)x, GY0 + (int)y, c, 12);
        PB(GX0 + (int)x, GY0 + (int)y - 1, c, 6);
        PB(GX0 + (int)x, GY0 + (int)y - 3, c, 4);
    }
}
static void buildings(int seed, int n, int ybase, int hmin, int hmax, uint16_t c, uint16_t lit, uint16_t dark, int density)
{
    int i, x = -3;
    for (i = 0; i < n && x < GW; i++) {
        int w = 7 + (int)(hf((uint32_t)(seed + i * 3)) * 8.0f), h = hmin + (int)(hf((uint32_t)(seed + i * 5 + 1)) * (float)(hmax - hmin));
        int bx = GX0 + x, by = GY0 + ybase - h, wx, wy;
        R(bx, by, w, h + 10, c);
        if (hf((uint32_t)(seed + i * 9)) < 0.3f)                 /* a little roof thing */
            R(bx + w / 2, by - 2, 1, 2, c);
        for (wy = by + 2; wy < GY0 + ybase - 1; wy += 3)
            for (wx = bx + 1; wx < bx + w - 1; wx += 2) {
                uint32_t hh = (uint32_t)(wx * 131 + wy * 17 + seed);
                float on = hf(hh + (uint32_t)(ui.t * 0.25f + hf(hh) * 4.0f) * 1013u);
                if (on * 100.0f < (float)density)
                    P(wx, wy, lit);
                else if (dark)
                    P(wx, wy, dark);
            }
        x += w + (int)(hf((uint32_t)(seed + i * 7)) * 3.0f);
    }
}
static void view_rain(void)
{
    grad(GX0, GX1, GY0, GY1, C565(16, 18, 42), C565(58, 54, 96), 6);
    disc(GX0 + 66, GY0 + 9, 5.0f, C565(70, 66, 110));             /* a moon behind the cloud */
    RB(GX0, GY0 + 6, GW, 6, C565(90, 86, 128), 5);
    buildings(11, 12, 50, 14, 34, C565(40, 40, 76), C565(240, 196, 110), C565(52, 52, 90), 26);
    buildings(77, 10, 58, 6, 18, C565(24, 24, 48), C565(255, 170, 90), 0, 18);
    R(GX0, GY0 + 52, GW, 4, C565(30, 30, 56));
    HL(GX0, GX1, GY0 + 52, C565(90, 80, 120));                     /* the wet street shines */
    rain_streaks(46, C565(150, 170, 220), 70.0f, 4);
    glass_drops(C565(170, 190, 240));
}
static void view_sunset(void)
{
    int x, i;
    grad(GX0, GX1, GY0, GY1, C565(244, 98, 98), C565(255, 210, 130), 7);
    /* the sun, with the stripes of an old poster */
    disc(GX0 + 52, GY0 + 34, 13.0f, C565(255, 236, 170));
    for (i = 0; i < 4; i++)
        RB(GX0 + 36, GY0 + 30 + i * 3, 34, 1 + i / 2, C565(255, 160, 110), 12);
    for (i = 0; i < 3; i++) {             /* clouds */
        float cx = fwrap(hf(i * 3u) * 100.0f + ui.t * (1.5f + (float)i), (float)GW + 30.0f) - 15.0f;
        int cy = 8 + i * 6;
        R(GX0 + (int)cx, GY0 + cy, 14 + i * 4, 1, C565(255, 168, 150));
        R(GX0 + (int)cx + 3, GY0 + cy - 1, 7 + i * 2, 1, C565(255, 190, 170));
    }
    for (x = 0; x < GW; x++) {            /* three hills */
        float fx = (float)x;
        int h1 = 36 + (int)(4.0f * sn(fx * 0.012f + 0.1f) + 2.0f * sn(fx * 0.031f));
        int h2 = 42 + (int)(5.0f * sn(fx * 0.017f + 0.6f));
        int h3 = 49 + (int)(3.0f * sn(fx * 0.024f + 0.3f));
        VL(GX0 + x, GY0 + h1, GY1 - 1, C565(196, 100, 120));
        VL(GX0 + x, GY0 + h2, GY1 - 1, C565(140, 66, 104));
        VL(GX0 + x, GY0 + h3, GY1 - 1, C565(86, 42, 82));
    }
    for (i = 0; i < 3; i++) {             /* birds, flapping */
        float bx = fwrap(hf(i * 5u + 2u) * 90.0f + ui.t * 5.0f, (float)GW + 20.0f) - 10.0f;
        int by = 14 + (int)(hf(i * 9u) * 12.0f) + (int)(1.5f * sn(ui.t * 0.3f + (float)i));
        int up = (int)(ui.t * 4.0f + (float)i) & 1;
        P(GX0 + (int)bx, GY0 + by, C565(90, 40, 60));
        P(GX0 + (int)bx - 1, GY0 + by - up, C565(90, 40, 60));
        P(GX0 + (int)bx + 1, GY0 + by - up, C565(90, 40, 60));
        P(GX0 + (int)bx - 2, GY0 + by - 1 + up, C565(90, 40, 60));
        P(GX0 + (int)bx + 2, GY0 + by - 1 + up, C565(90, 40, 60));
    }
}
static void pine(int x, int y, int h, uint16_t c, uint16_t snow)
{
    int k;
    for (k = 0; k < 3; k++) {
        int ty = y - h + k * h / 3, w = 3 + k * 2 + h / 6;
        tri(x, ty, x - w, ty + h / 2, x + w, ty + h / 2, c);
        HL(x - 1 - k, x + 1 + k, ty + 2, snow);
        P(x, ty, snow);
    }
    R(x - 1, y - 2, 2, 3, C565(60, 40, 34));
}
static void view_snow(void)
{
    int i, x;
    grad(GX0, GX1, GY0, GY1, C565(22, 30, 64), C565(80, 100, 150), 6);
    disc(GX0 + 64, GY0 + 11, 6.0f, C565(232, 236, 250));
    P(GX0 + 62, GY0 + 10, C565(200, 206, 230));
    P(GX0 + 66, GY0 + 13, C565(200, 206, 230));
    for (i = 0; i < 20; i++)
        if (sn(ui.t * 0.2f + hf(i * 7u)) > 0.2f)
            P(GX0 + (int)(hf(i * 19u) * (float)GW), GY0 + (int)(hf(i * 23u) * 30.0f), C565(200, 210, 240));
    for (x = 0; x < GW; x++) {
        int h = 38 + (int)(4.0f * sn((float)x * 0.015f + 0.2f));
        VL(GX0 + x, GY0 + h, GY1 - 1, C565(150, 166, 204));
    }
    for (i = 0; i < 7; i++)
        pine(GX0 + 4 + i * 12 + (int)(hf(i * 3u) * 5.0f), GY0 + 48 + (int)(hf(i * 5u) * 4.0f), 16 + (int)(hf(i * 7u) * 10.0f),
             C565(28, 62, 72), C565(220, 232, 250));
    R(GX0, GY0 + 50, GW, GH - 50, C565(214, 224, 244));
    for (x = 0; x < GW; x += 3)
        P(GX0 + x + ((x / 3) & 1), GY0 + 51, C565(190, 204, 232));
    /* a little cabin far away, its window lit */
    R(GX0 + 70, GY0 + 44, 8, 6, C565(90, 56, 44));
    tri(GX0 + 69, GY0 + 44, GX0 + 74, GY0 + 40, GX0 + 79, GY0 + 44, C565(230, 236, 250));
    P(GX0 + 73, GY0 + 46, (int)(ui.t * 2.0f) % 7 ? C565(255, 210, 120) : C565(255, 180, 90));
    for (i = 0; i < 46; i++) {           /* snow falling */
        float sp = 4.0f + hf(i * 3u) * 6.0f;
        float y = fwrap(hf(i * 11u) * 70.0f + ui.t * sp, (float)GH + 4.0f) - 2.0f;
        float x0 = hf(i * 17u) * (float)GW + 2.0f * sn(ui.t * 0.15f + hf(i * 5u));
        P(GX0 + (int)x0, GY0 + (int)y, C565(250, 252, 255));
        if (i % 6 == 0)
            P(GX0 + (int)x0 + 1, GY0 + (int)y, C565(250, 252, 255));
    }
}
static void neon_sign(int x, int y, int w, int h, uint16_t c, int on)
{
    uint16_t cc = on ? c : mix(c, C565(30, 20, 40), 11);
    HL(x, x + w - 1, y, cc);
    HL(x, x + w - 1, y + h - 1, cc);
    VL(x, y, y + h - 1, cc);
    VL(x + w - 1, y, y + h - 1, cc);
    if (on) {
        RB(x - 1, y - 1, w + 2, h + 2, c, 3);
        R(x + 2, y + 2, w - 4, 1, c);
    }
}
static void view_neon(void)
{
    int on = (int)(ui.t * 7.0f);
    grad(GX0, GX1, GY0, GY1, C565(26, 10, 46), C565(96, 28, 100), 6);
    buildings(41, 12, 50, 18, 38, C565(44, 22, 70), C565(255, 120, 210), C565(56, 30, 84), 14);
    buildings(93, 10, 58, 10, 26, C565(22, 12, 40), C565(120, 220, 255), 0, 16);
    neon_sign(GX0 + 8, GY0 + 26, 12, 6, C565(255, 90, 190), (hsh((uint32_t)on) & 7u) != 0u);
    neon_sign(GX0 + 34, GY0 + 32, 8, 10, C565(90, 230, 255), 1);
    neon_sign(GX0 + 58, GY0 + 22, 14, 5, C565(255, 210, 90), kick_pulse > 0.3f || !playing);
    neon_sign(GX0 + 70, GY0 + 36, 6, 8, C565(160, 255, 140), (hsh((uint32_t)on + 99u) & 15u) != 0u);
    R(GX0, GY0 + 52, GW, GH - 52, C565(20, 10, 36));
    RB(GX0 + 8, GY0 + 53, 12, 1, C565(255, 90, 190), 6);           /* the signs in the puddles */
    RB(GX0 + 34, GY0 + 54, 8, 1, C565(90, 230, 255), 6);
    rain_streaks(22, C565(200, 140, 220), 60.0f, 3);
}
static void view_beach(void)
{
    int i, y, x;
    float sh;
    grad(GX0, GX1, GY0, GY0 + 34, C565(8, 12, 38), C565(46, 50, 104), 5);
    for (i = 0; i < 34; i++) {           /* stars, twinkling */
        float tw = sn(ui.t * (0.3f + hf(i * 3u) * 0.5f) + hf(i * 5u));
        if (tw > -0.3f)
            P(GX0 + (int)(hf(i * 7u + 1u) * (float)GW), GY0 + (int)(hf(i * 13u + 2u) * 30.0f),
              tw > 0.7f ? C565(255, 255, 230) : C565(150, 150, 190));
    }
    /* a shooting star now and then */
    sh = fwrap(ui.t, 9.0f);
    if (sh < 0.6f) {
        int sx = GX0 + 70 - (int)(sh * 80.0f), sy = GY0 + 4 + (int)(sh * 20.0f);
        for (i = 0; i < 5; i++)
            PB(sx + i, sy - i / 2, C565(255, 255, 240), 14 - i * 3);
    }
    disc(GX0 + 22, GY0 + 13, 7.0f, C565(244, 240, 214));
    P(GX0 + 19, GY0 + 11, C565(214, 210, 186));
    P(GX0 + 24, GY0 + 15, C565(214, 210, 186));
    P(GX0 + 23, GY0 + 10, C565(214, 210, 186));
    grad(GX0, GX1, GY0 + 34, GY0 + 50, C565(24, 40, 86), C565(12, 22, 54), 4);
    for (y = 35; y < 50; y += 2) {       /* the moon on the water: a few shimmering dashes */
        int w = 1 + (y - 35) / 4;
        int off = (int)(1.6f * sn(ui.t * 0.35f + (float)y * 0.21f));
        if (((y + (int)(ui.t * 2.0f)) % 5) == 0)
            continue;
        for (x = -w; x <= w; x++)
            PB(GX0 + 22 + x + off, GY0 + y, C565(240, 236, 200), 10 - (x < 0 ? -x : x) * 2);
    }
    for (x = 0; x < GW; x++) {           /* the foam comes and goes */
        float s = 50.0f + 1.5f * sn(ui.t * 0.11f) + 0.8f * sn((float)x * 0.05f + ui.t * 0.3f);
        P(GX0 + x, GY0 + (int)s, C565(170, 196, 230));
        R(GX0 + x, GY0 + (int)s + 1, 1, GH, C565(60, 60, 92));
    }
    /* a palm on the right */
    for (i = 0; i < 16; i++)
        P(GX0 + 70 + i / 4, GY0 + 52 - i, C565(30, 24, 40));
    for (i = -1; i <= 1; i += 2) {
        int k;
        for (k = 0; k < 7; k++) {
            float sway = sn(ui.t * 0.2f) * 0.6f;
            P(GX0 + 74 + i * k, GY0 + 36 + k / 2 + (int)(sway * (float)k * 0.3f), C565(30, 24, 40));
            P(GX0 + 74 + i * k, GY0 + 37 + k / 2 + k / 3, C565(30, 24, 40));
        }
    }
}
static void tree_round(int x, int y, int r, uint16_t c1, uint16_t c2, uint16_t c3)
{
    R(x - 1, y, 3, 14, C565(96, 62, 42));
    disc((float)x, (float)(y - 2), (float)r, c1);
    disc((float)(x - r / 2), (float)(y), (float)r * 0.7f, c2);
    disc((float)(x + r / 2), (float)(y - r / 3), (float)r * 0.6f, c3);
    P(x - 2, y - r, c3);
    P(x + 3, y - 2, c3);
}
static void view_autumn(void)
{
    int i, x;
    grad(GX0, GX1, GY0, GY1, C565(110, 170, 226), C565(204, 226, 244), 6);
    for (i = 0; i < 3; i++) {             /* clouds drifting */
        float cx = fwrap(hf(i * 3u + 8u) * 100.0f + ui.t * (1.0f + (float)i * 0.5f), (float)GW + 30.0f) - 15.0f;
        int cy = 7 + i * 7;
        disc(GX0 + cx, GY0 + (float)cy, 4.0f, C565(248, 250, 255));
        disc(GX0 + cx + 5.0f, GY0 + (float)cy - 1.0f, 5.0f, C565(248, 250, 255));
        disc(GX0 + cx + 10.0f, GY0 + (float)cy, 3.5f, C565(248, 250, 255));
        R(GX0 + (int)cx - 2, GY0 + cy + 2, 16, 2, C565(220, 228, 240));
    }
    for (x = 0; x < GW; x++) {
        int h = 40 + (int)(3.0f * sn((float)x * 0.02f + 0.4f));
        VL(GX0 + x, GY0 + h, GY1 - 1, C565(150, 170, 110));
    }
    tree_round(GX0 + 14, GY0 + 40, 9, C565(230, 120, 50), C565(200, 80, 40), C565(250, 180, 70));
    tree_round(GX0 + 40, GY0 + 44, 7, C565(240, 170, 60), C565(220, 120, 50), C565(255, 210, 100));
    tree_round(GX0 + 66, GY0 + 39, 10, C565(210, 90, 50), C565(180, 60, 40), C565(240, 150, 60));
    R(GX0, GY0 + 52, GW, GH - 52, C565(176, 132, 74));
    HL(GX0, GX1, GY0 + 52, C565(150, 110, 60));
    for (i = 0; i < 18; i++) {           /* leaves falling, swaying */
        static const uint16_t LC[3] = {C565(240, 140, 50), C565(210, 70, 40), C565(255, 200, 80)};
        float sp = 3.0f + hf(i * 3u) * 4.0f;
        float y = fwrap(hf(i * 11u) * 70.0f + ui.t * sp, (float)GH + 4.0f) - 2.0f;
        float xx = fwrap(hf(i * 17u) * (float)GW + 4.0f * sn(ui.t * 0.25f + hf(i * 5u)) + ui.t * 2.0f, (float)GW);
        P(GX0 + (int)xx, GY0 + (int)y, LC[i % 3]);
        if (sn(ui.t * 0.5f + (float)i * 0.3f) > 0.0f)
            P(GX0 + (int)xx + 1, GY0 + (int)y, LC[i % 3]);
    }
}
static void view_sakura(void)
{
    int i, x;
    grad(GX0, GX1, GY0, GY1, C565(252, 196, 210), C565(196, 214, 250), 6);
    /* the mountain */
    tri(GX0 + 12, GY0 + 46, GX0 + 44, GY0 + 12, GX0 + 78, GY0 + 46, C565(140, 150, 206));
    tri(GX0 + 37, GY0 + 20, GX0 + 44, GY0 + 12, GX0 + 51, GY0 + 20, C565(250, 250, 255));
    P(GX0 + 39, GY0 + 21, C565(250, 250, 255));
    P(GX0 + 43, GY0 + 22, C565(250, 250, 255));
    P(GX0 + 48, GY0 + 21, C565(250, 250, 255));
    for (x = 0; x < GW; x++) {
        int h = 44 + (int)(2.0f * sn((float)x * 0.03f));
        VL(GX0 + x, GY0 + h, GY1 - 1, C565(120, 170, 120));
    }
    R(GX0, GY0 + 50, GW, GH - 50, C565(150, 190, 220));             /* a lake */
    for (x = 0; x < GW; x += 2)
        if (((x + (int)(ui.t * 2.0f)) % 7) == 0)
            P(GX0 + x, GY0 + 52 + (x % 3), C565(220, 236, 250));
    /* cherry trees */
    R(GX0 + 7, GY0 + 34, 2, 16, C565(90, 56, 60));
    disc(GX0 + 8.0f, GY0 + 30.0f, 9.0f, C565(250, 170, 200));
    disc(GX0 + 3.0f, GY0 + 33.0f, 5.0f, C565(236, 140, 176));
    disc(GX0 + 14.0f, GY0 + 32.0f, 5.0f, C565(255, 196, 216));
    R(GX0 + 72, GY0 + 36, 2, 14, C565(90, 56, 60));
    disc(GX0 + 73.0f, GY0 + 32.0f, 8.0f, C565(250, 170, 200));
    disc(GX0 + 78.0f, GY0 + 35.0f, 5.0f, C565(236, 140, 176));
    for (i = 0; i < 22; i++) {           /* petals on the wind */
        float sp = 3.0f + hf(i * 3u) * 3.0f;
        float y = fwrap(hf(i * 11u) * 70.0f + ui.t * sp, (float)GH + 4.0f) - 2.0f;
        float xx = fwrap(hf(i * 17u) * (float)GW + ui.t * (5.0f + sp) + 3.0f * sn(ui.t * 0.3f + hf(i * 5u)), (float)GW);
        P(GX0 + (int)xx, GY0 + (int)y, i & 1 ? C565(255, 200, 220) : C565(250, 160, 196));
    }
}
static void view_orbit(void)
{
    int i, x, y;
    grad(GX0, GX1, GY0, GY1, C565(4, 4, 14), C565(20, 14, 44), 4);
    for (i = 0; i < 48; i++) {           /* stars, two depths drifting */
        float sp = i < 30 ? 0.6f : 1.8f;
        float sx = fwrap(hf(i * 7u + 3u) * 100.0f - ui.t * sp, (float)GW);
        P(GX0 + (int)sx, GY0 + (int)(hf(i * 13u) * (float)GH), i < 30 ? C565(110, 110, 150) : C565(230, 230, 255));
    }
    /* the planet below, turning */
    for (y = 34; y < GH; y++)
        for (x = 0; x < GW; x++) {
            float dx = (float)x - 41.0f, dy = (float)y - 84.0f, d2 = dx * dx + dy * dy;
            if (d2 < 52.0f * 52.0f) {
                float u = (float)x * 0.06f + ui.t * 0.05f, v = (float)y * 0.09f;
                float land = sn(u * 0.7f + 0.3f * sn(v)) + sn(v * 0.8f + u * 0.3f) * 0.8f;
                uint16_t c = land > 0.6f ? C565(90, 170, 100) : C565(40, 96, 190);
                if (d2 > 50.0f * 50.0f)
                    c = C565(150, 210, 255);
                else if (land > 1.2f)
                    c = C565(230, 236, 240);       /* clouds */
                P(GX0 + x, GY0 + y, c);
            }
        }
    /* a satellite */
    {
        float sx = fwrap(ui.t * 3.0f, (float)GW + 20.0f) - 10.0f, sy = 14.0f + 3.0f * sn(ui.t * 0.05f);
        int ix = GX0 + (int)sx, iy = GY0 + (int)sy;
        R(ix - 4, iy, 3, 2, C565(80, 120, 200));
        R(ix + 2, iy, 3, 2, C565(80, 120, 200));
        R(ix - 1, iy, 2, 2, C565(220, 220, 230));
        if ((int)(ui.t * 2.0f) & 1)
            P(ix, iy - 1, C565(255, 80, 80));
    }
}

static void draw_view(int th)
{
    clip(GX0, GY0, GX1, GY1);
    switch (th) {
    case TH_RAIN: view_rain(); break;
    case TH_SUNSET: view_sunset(); break;
    case TH_SNOW: view_snow(); break;
    case TH_NEON: view_neon(); break;
    case TH_BEACH: view_beach(); break;
    case TH_AUTUMN: view_autumn(); break;
    case TH_SAKURA: view_sakura(); break;
    default: view_orbit(); break;
    }
    /* the glass: a soft shine across it */
    {
        int i;
        for (i = 0; i < 26; i++) {
            PB(GX0 + 4 + i, GY0 + 30 - i, C565(255, 255, 255), 2);
            PB(GX0 + 8 + i, GY0 + 30 - i, C565(255, 255, 255), 2);
            PB(GX0 + 48 + i, GY0 + 52 - i, C565(255, 255, 255), 2);
        }
    }
    noclip();
}

/* --------------------------------------------------------- the room --- */
static void draw_wall(const room_t *r)
{
    int x, y;
    for (y = 0; y < 84; y++)
        for (x = 0; x < LW; x++) {
            uint16_t c = r->wall;
            switch (r->wallpat) {
            case 0: if ((x % 8) < 2) c = r->wall2; break;                    /* stripes */
            case 1: if (y % 9 == 0 || ((x + (y / 9) * 13) % 31) == 0) c = r->wall2; break;   /* planks */
            case 2: if ((x % 8) == 3 && (y % 8) == ((x / 8) & 1) * 4) c = r->wall2; break;   /* dots */
            default: if (y % 20 == 0 || x % 30 == 0) c = r->wall2;                         /* panels, rivets */
                     else if ((y % 20 == 3 || y % 20 == 17) && (x % 30 == 3 || x % 30 == 27)) c = r->wall2;
                     break;
            }
            lofb[y * LW + x] = c;
        }
}

static void draw_window(const room_t *r)
{
    int th = theme_now();
    /* frame */
    R(16, 16, 88, 3, r->frame);
    R(16, 75, 88, 3, r->frame);
    R(16, 16, 3, 62, r->frame);
    R(101, 16, 3, 62, r->frame);
    HL(16, 103, 16, r->frame_dk);
    VL(16, 16, 77, r->frame_dk);
    draw_view(th);
    /* the cross */
    R(59, GY0, 2, GH, r->frame);
    R(GX0, 46, GW, 2, r->frame);
    VL(60, GY0, GY1 - 1, r->frame_dk);
    HL(GX0, GX1 - 1, 47, r->frame_dk);
    /* inner shadow */
    HL(GX0, GX1 - 1, GY0, mix(r->frame_dk, 0, 6));
    VL(GX0, GY0, GY1 - 1, mix(r->frame_dk, 0, 6));
}

static void draw_curtains(const room_t *r)
{
    int y, side;
    R(8, 12, 104, 2, r->frame_dk);                          /* the rod */
    disc(8.0f, 13.0f, 1.6f, r->frame);
    disc(111.0f, 13.0f, 1.6f, r->frame);
    for (side = 0; side < 2; side++)
        for (y = 14; y < 82; y++) {
            /* wide at the top, gathered at the tie, falling out below */
            float ty = (float)(y - 50);
            int w = y < 50 ? 10 - (y - 14) / 7 : 5 + (y - 50) / 5;
            int x0, x, sway = (int)(0.6f * sn(ui.t * 0.1f + (float)y * 0.02f + (float)side * 0.5f));
            (void)ty;
            if (w > 11) w = 11;
            x0 = side ? 111 - w + sway : 8 + sway;
            for (x = x0; x < x0 + w; x++) {
                int f = (x - x0 + y / 16) % 4;
                P(x, y, f == 0 ? r->curtain_dk : r->curtain);
            }
            if (y == 50)
                R(x0, y, w, 2, r->accent);                  /* the tie */
        }
}

static void draw_sill(const room_t *r)
{
    R(10, 78, 100, 4, r->sill);
    HL(10, 109, 78, mix(r->sill, C565(255, 255, 255), 4));
    HL(10, 109, 82, r->frame_dk);
}

/* ------------------------------------------------------ the cat --- */
typedef struct {
    uint16_t fur, light, dark, line, eye;
} coat_t;
static const coat_t COATS[4] = {
    {C565(236, 148, 70), C565(252, 222, 176), C565(198, 106, 46), C565(96, 50, 30), C565(60, 40, 30)},
    {C565(44, 42, 54), C565(74, 72, 88), C565(30, 28, 38), C565(14, 14, 20), C565(210, 230, 90)},
    {C565(150, 152, 168), C565(214, 214, 224), C565(118, 120, 136), C565(62, 62, 76), C565(70, 60, 50)},
    {C565(246, 240, 230), C565(255, 252, 246), C565(232, 140, 64), C565(80, 64, 60), C565(60, 50, 40)},
};
static float bob, ear_twitch, blink_t, tail_amt;

static void draw_cat(int cx, int by)
{
    const coat_t *c = &COATS[set.cat];
    float hy = (float)by - 15.0f + bob * 1.2f + (playing ? 0.0f : 1.0f);
    float bp = playing ? beat_pos : ui.t * 0.3f;
    int i, eyes_shut = !playing || blink_t > 0.0f, hyi = (int)(hy + 0.5f);
    /* the tail, behind: a curve that swishes with the beat */
    for (i = 0; i < 14; i++) {
        float s = (float)i / 13.0f, a = 0.10f + s * 0.26f + tail_amt * 0.10f * sn(bp * 0.5f) * s;
        float tx = (float)cx + 7.0f + 8.0f * s * fm_cosf(a * 6.2831f) * 0.9f;
        float ty = (float)by - 2.0f - 13.0f * s * fm_sinf(a * 6.2831f);
        disc(tx, ty, 1.9f, c->line);
    }
    for (i = 0; i < 14; i++) {
        float s = (float)i / 13.0f, a = 0.10f + s * 0.26f + tail_amt * 0.10f * sn(bp * 0.5f) * s;
        float tx = (float)cx + 7.0f + 8.0f * s * fm_cosf(a * 6.2831f) * 0.9f;
        float ty = (float)by - 2.0f - 13.0f * s * fm_sinf(a * 6.2831f);
        disc(tx, ty, 1.1f, set.cat == 3 && i > 9 ? c->dark : i > 10 && set.cat == 0 ? c->dark : c->fur);
    }
    /* body */
    ell((float)cx, (float)by - 6.0f, 8.6f, 6.8f, c->line);
    ell((float)cx, (float)by - 6.0f, 7.6f, 5.9f, c->fur);
    ell((float)cx, (float)by - 4.0f, 4.0f, 3.8f, c->light);
    if (set.cat == 0) {                 /* the ginger's stripes */
        HL(cx - 7, cx - 5, by - 8, c->dark);
        HL(cx + 5, cx + 7, by - 8, c->dark);
        HL(cx - 7, cx - 5, by - 5, c->dark);
        HL(cx + 5, cx + 7, by - 5, c->dark);
    } else if (set.cat == 3) {          /* the calico's patches */
        ell((float)cx - 5.0f, (float)by - 8.0f, 2.5f, 2.0f, c->dark);
        ell((float)cx + 5.0f, (float)by - 5.0f, 2.0f, 2.0f, C565(60, 50, 50));
    }
    /* paws */
    ell((float)cx - 3.0f, (float)by - 0.8f, 2.2f, 1.4f, c->line);
    ell((float)cx + 3.0f, (float)by - 0.8f, 2.2f, 1.4f, c->line);
    ell((float)cx - 3.0f, (float)by - 1.0f, 1.6f, 1.0f, c->light);
    ell((float)cx + 3.0f, (float)by - 1.0f, 1.6f, 1.0f, c->light);
    /* ears */
    {
        int et = ear_twitch > 0.0f ? 1 : 0;
        tri(cx - 7, hyi - 2, cx - 6 - et, hyi - 9, cx - 2, hyi - 4, c->line);
        tri(cx + 7, hyi - 2, cx + 6, hyi - 9, cx + 2, hyi - 4, c->line);
        tri(cx - 6, hyi - 3, cx - 5 - et, hyi - 7, cx - 3, hyi - 4, C565(240, 150, 160));
        tri(cx + 6, hyi - 3, cx + 5, hyi - 7, cx + 3, hyi - 4, C565(240, 150, 160));
    }
    /* head */
    ell((float)cx, hy, 7.8f, 6.0f, c->line);
    ell((float)cx, hy, 6.9f, 5.1f, c->fur);
    if (set.cat == 3)
        ell((float)cx + 3.5f, hy - 2.0f, 3.0f, 2.4f, c->dark);
    if (set.cat == 0) {
        P(cx, hyi - 4, c->dark);
        P(cx - 1, hyi - 4, c->dark);
        P(cx + 1, hyi - 4, c->dark);
        P(cx, hyi - 3, c->dark);
    }
    ell((float)cx, hy + 2.0f, 3.4f, 2.0f, c->light);
    /* face */
    if (eyes_shut) {
        HL(cx - 4, cx - 2, hyi, c->line);
        HL(cx + 2, cx + 4, hyi, c->line);
        P(cx - 3, hyi + 1, c->fur);
    } else {
        R(cx - 4, hyi - 1, 2, 2, c->eye);
        R(cx + 3, hyi - 1, 2, 2, c->eye);
        P(cx - 4, hyi - 1, C565(255, 255, 255));
        P(cx + 3, hyi - 1, C565(255, 255, 255));
    }
    P(cx, hyi + 1, C565(236, 120, 140));                 /* nose */
    P(cx - 1, hyi + 2, c->line);
    P(cx + 1, hyi + 2, c->line);
    PB(cx - 5, hyi + 2, C565(255, 120, 140), 8);          /* cheeks */
    PB(cx + 5, hyi + 2, C565(255, 120, 140), 8);
    HL(cx - 10, cx - 7, hyi + 1, mix(c->line, c->light, 8));
    HL(cx + 7, cx + 10, hyi + 1, mix(c->line, c->light, 8));
}

/* ------------------------------------------------------ the sill's things --- */
static void draw_plant(int x, int by)
{
    int i;
    float sw = 0.8f * sn(ui.t * 0.17f) + (playing ? 0.4f * kick_pulse : 0.0f);
    for (i = 0; i < 5; i++) {
        float a = -0.18f + (float)i * 0.09f;
        int k;
        for (k = 0; k < 10 - (i == 2 ? 0 : 3); k++) {
            float s = (float)k;
            int lx = x + 4 + (int)(s * a * 1.4f + sw * s * 0.08f), ly = by - 8 - k;
            P(lx, ly, k > 4 ? C565(110, 190, 90) : C565(70, 150, 70));
            if (k > 3 && k < 8)
                P(lx + (a < 0.0f ? -1 : 1), ly, C565(90, 170, 80));
        }
    }
    R(x, by - 8, 9, 2, C565(196, 96, 60));
    R(x + 1, by - 6, 7, 6, C565(176, 82, 52));
    HL(x + 1, x + 7, by - 6, C565(140, 64, 42));
}

static void draw_mug(int x, int by, const room_t *r)
{
    int i;
    R(x, by - 7, 7, 7, C565(240, 236, 226));
    R(x, by - 7, 7, 1, C565(90, 56, 40));
    VL(x + 7, by - 5, by - 3, C565(240, 236, 226));
    P(x + 8, by - 4, C565(240, 236, 226));
    R(x + 2, by - 4, 3, 2, r->accent);                    /* a little heart on it */
    P(x + 3, by - 2, r->accent);
    for (i = 0; i < 2; i++) {           /* steam */
        int k;
        for (k = 0; k < 9; k++) {
            float ph = ui.t * 0.6f + (float)i * 0.5f;
            int sy = by - 9 - k, sx = x + 2 + i * 3 + (int)(1.4f * sn(ph - (float)k * 0.08f));
            int fade = 9 - k;
            if (((k + (int)(ui.t * 6.0f)) % 5) == 0)
                continue;
            PB(sx, sy, C565(255, 255, 255), fade);
        }
    }
}

/* ------------------------------------------------- the desk and the deck --- */
static float reel_a;
static struct {
    float x, y, life;
} notes[8];
static void note_spawn(void)
{
    int i;
    for (i = 0; i < 8; i++)
        if (notes[i].life <= 0.0f) {
            notes[i].x = 20.0f + hf((uint32_t)(ui.t * 1000.0f)) * 8.0f;
            notes[i].y = 88.0f;
            notes[i].life = 1.0f;
            return;
        }
}
static void draw_note_glyph(int x, int y, uint16_t c, int t)
{
    PB(x, y + 3, c, t);
    PB(x + 1, y + 3, c, t);
    PB(x, y + 4, c, t);
    PB(x + 1, y + 4, c, t);
    PB(x + 2, y, c, t);
    PB(x + 2, y + 1, c, t);
    PB(x + 2, y + 2, c, t);
    PB(x + 2, y + 3, c, t);
    PB(x + 3, y, c, t);
    PB(x + 4, y + 1, c, t);
}

static void reel(int cx, int cy, float a, uint16_t hub, uint16_t ring)
{
    int k;
    disc((float)cx + 0.5f, (float)cy + 0.5f, 3.6f, ring);
    disc((float)cx + 0.5f, (float)cy + 0.5f, 2.4f, hub);
    for (k = 0; k < 3; k++) {
        float aa = a + (float)k / 3.0f;
        P(cx + (int)(2.4f * fm_cosf(aa * 6.2831f) + 0.5f), cy + (int)(2.4f * fm_sinf(aa * 6.2831f) + 0.5f), ring);
    }
    P(cx, cy, ring);
}

static void draw_desk(const room_t *r)
{
    int x, y;
    R(0, 84, LW, 36, r->desk);
    HL(0, LW - 1, 84, mix(r->desk, C565(255, 255, 255), 4));
    for (y = 88; y < LH; y += 6)        /* wood grain */
        for (x = (y * 7) % 13; x < LW; x += 17)
            HL(x, x + 5, y, r->desk_dk);
    /* the cassette deck */
    R(3, 88, 32, 26, C565(40, 38, 46));
    R(4, 89, 30, 24, C565(64, 62, 72));
    R(6, 91, 26, 15, C565(28, 26, 32));                   /* the cassette window */
    R(7, 92, 24, 5, C565(236, 228, 210));                 /* the label */
    R(7, 92, 24, 1, r->accent);
    R(7, 96, 24, 1, r->accent2);
    reel(13, 101, reel_a, C565(236, 228, 210), C565(90, 70, 60));
    reel(25, 101, reel_a + 0.17f, C565(236, 228, 210), C565(90, 70, 60));
    HL(16, 22, 104, C565(70, 50, 40));                    /* the tape */
    for (x = 0; x < 4; x++)             /* the keys of the deck: PLAY down while playing */
        R(6 + x * 7, 108 + (x == 1 && playing ? 1 : 0), 5, 3 - (x == 1 && playing ? 1 : 0),
          x == 1 && playing ? C565(120, 220, 140) : C565(150, 148, 160));
    if (playing && ((int)(ui.t * 2.0f) & 1))
        P(31, 109, C565(255, 80, 80));                    /* the recording light, for fun */
}

/* --------------------------------------------------- the panels and the cards --- */
static void panel(int x, int y, int w, int h, uint16_t bg, uint16_t edge)
{
    R(x + 1, y, w - 2, h, bg);
    R(x, y + 1, w, h - 2, bg);
    HL(x + 1, x + w - 2, y, edge);
    HL(x + 1, x + w - 2, y + h - 1, mix(edge, 0, 8));
    VL(x, y + 1, y + h - 2, edge);
    VL(x + w - 1, y + 1, y + h - 2, mix(edge, 0, 8));
}

static void draw_info_panel(const room_t *r)
{
    const mood_t *m = &MOODS[playing ? eng_stat.mood : set.mood];
    int i, n = m->n, cur = playing ? eng_stat.chord : -1, w = n > 4 ? 3 : 5;
    panel(39, 87, 79, 31, r->panel, mix(r->accent, r->panel, 6));
    /* the loop's chords, the one now lit */
    for (i = 0; i < n; i++) {
        int x = 116 - (n - i) * (w + 1);
        R(x, 90, w, 3, i == cur ? r->accent : mix(r->accent, r->panel, 11));
    }
    /* the beat */
    for (i = 0; i < 4; i++) {
        int on = playing && ui.step >= 0 && ui.step / 4 == i;
        R(103 + i * 3, 113, 2, 2, on ? r->accent2 : mix(r->accent2, r->panel, 12));
    }
    /* the drum and key icons */
    R(42, 111, 5, 3, mix(r->accent2, r->panel, 4));       /* a little snare drum */
    HL(42, 46, 110, C565(230, 230, 230));
    P(41, 108, C565(200, 200, 200));
    P(47, 108, C565(200, 200, 200));
    /* the top bar */
    RB(0, 0, LW, 11, r->panel, 13);
    HL(0, LW - 1, 11, mix(r->panel, r->accent, 4));
    if (playing) {                      /* play: a triangle; stopped: two bars */
        tri(3, 3, 3, 7, 6, 5, r->accent);
    } else {
        R(3, 3, 1, 5, r->accent);
        R(5, 3, 1, 5, r->accent);
    }
}

static void card_knobs(const room_t *r)
{
    int k;
    panel(14, 15, 92, 56, r->panel, r->accent);
    for (k = 0; k < 4; k++) {
        const kdef_t *d = &KD[set.page][k];
        int y = 29 + k * 11, w = (int)((float)(*d->v - d->lo) / (float)(d->hi - d->lo) * 26.0f + 0.5f);
        uint16_t c = k == ui.knob_hi ? r->accent : mix(r->accent, r->panel, 6);
        R(62, y, 26, 2, mix(r->accent, r->panel, 13));
        R(62, y, w, 2, c);
    }
}
static void card_banner(const room_t *r) { panel(10, 22, 100, 36, r->panel, r->accent); }
static void card_help(const room_t *r) { panel(3, 3, 114, 114, r->panel, r->accent); }

static void card_scale(const room_t *r)
{
    /* a fretboard: the high E on top, frets 0..12; the scale's notes, the roots lit, the chord's tones ringed */
    static const uint8_t OPEN[6] = {4, 11, 7, 2, 9, 4};   /* E B G D A E, high to low */
    const scale_t *s = &SCALES[MOODS[set.mood].scale];
    int st, f, x0 = 12, y0 = 32, fw = 7, sh = 5;
    panel(4, 17, 112, 56, r->panel, r->accent);
    for (f = 0; f <= 12; f++)
        VL(x0 + f * fw + 3, y0, y0 + sh * 5, f == 0 ? C565(230, 230, 230) : C565(110, 110, 120));
    for (st = 0; st < 6; st++)
        HL(x0, x0 + 12 * fw + 3, y0 + st * sh, C565(170, 160, 150));
    for (f = 3; f <= 12; f += 2) {
        if (f == 11)
            continue;
        P(x0 + f * fw, y0 + sh * 5 + 3, C565(160, 160, 170));
        if (f == 12)
            P(x0 + f * fw + 2, y0 + sh * 5 + 3, C565(160, 160, 170));
    }
    for (st = 0; st < 6; st++)
        for (f = 0; f <= 12; f++) {
            int pc = (OPEN[st] + f) % 12, d = ((pc - set.root) % 12 + 12) % 12, i, in = 0;
            int x = f == 0 ? x0 - 1 : x0 + f * fw - 1, y = y0 + st * sh - 1;
            for (i = 0; i < s->n; i++)
                if (s->iv[i] == d)
                    in = 1;
            if (!in)
                continue;
            if (d == 0) {
                R(x, y, 3, 3, r->accent);
            } else {
                R(x, y + 1, 3, 1, C565(240, 240, 240));
                R(x + 1, y, 1, 3, C565(240, 240, 240));
            }
            if (playing && (eng_stat.keys_mask >> pc & 1u) && d != 0)
                P(x + 1, y + 1, r->accent2);
        }
}

/* --------------------------------------------------- one frame of the picture --- */
static void scene_step(float dt)
{
    int i;
    uint32_t now = plat_ms();
    float spb = 60.0f / (float)(set.bpm ? set.bpm : 80);
    ui.t += dt;
    if (eng_stat.step != ui.step) {
        ui.step = eng_stat.step;
        ui.step_at = now;
    }
    if (playing && ui.step >= 0)
        beat_pos = (float)(eng_stat.bar * 4u) + (float)ui.step * 0.25f +
                   fm_minf((float)(now - ui.step_at) * 0.001f / spb, 0.25f);
    if (eng_stat.kicks != ui.kicks) {
        ui.kicks = eng_stat.kicks;
        kick_pulse = 1.0f;
        if (playing)
            bob = 1.0f;
    }
    if (eng_stat.snares != ui.snares) {
        ui.snares = eng_stat.snares;
        snare_pulse = 1.0f;
        if (hf(now) < 0.15f)
            ear_twitch = 0.25f;
    }
    if (eng_stat.keyhits != ui.keyhits) {
        if (playing && eng_stat.keyhits - ui.keyhits >= 2u && hf(now + 7u) < 0.35f)
            note_spawn();
        ui.keyhits = eng_stat.keyhits;
    }
    bob *= fm_expf(-dt * 9.0f);
    kick_pulse *= fm_expf(-dt * 7.0f);
    snare_pulse *= fm_expf(-dt * 7.0f);
    ear_twitch -= dt;
    tail_amt += ((playing ? 1.0f : 0.25f) - tail_amt) * fm_minf(dt * 2.0f, 1.0f);
    if (blink_t > 0.0f)
        blink_t -= dt;
    else if (hf((uint32_t)(ui.t * 10.0f) * 77u) < dt * 0.35f)
        blink_t = 0.14f;
    if (playing)
        reel_a = fwrap(reel_a + dt * 0.55f, 1.0f);
    for (i = 0; i < 8; i++)
        if (notes[i].life > 0.0f) {
            notes[i].life -= dt * 0.45f;
            notes[i].y -= dt * 9.0f;
            notes[i].x += dt * 3.0f * sn(notes[i].life * 0.8f);
        }
}

static void scene_draw(void)
{
    int th = theme_now(), i;
    const room_t *r = &ROOMS[th];
    noclip();
    draw_wall(r);
    draw_window(r);
    draw_curtains(r);
    draw_sill(r);
    draw_plant(20, 78);
    draw_mug(42, 78, r);
    draw_cat(82, 78);
    draw_desk(r);
    for (i = 0; i < 8; i++)
        if (notes[i].life > 0.0f)
            draw_note_glyph((int)notes[i].x, (int)notes[i].y, r->accent, (int)(notes[i].life * 16.0f));
    if (!playing) {                     /* asleep: z's float up and fade */
        int k;
        for (k = 0; k < 2; k++) {
            float z = fwrap(ui.t * 0.4f + (float)k * 0.5f, 1.0f);
            int zx = 89 + (int)(z * 7.0f), zy = 56 - (int)(z * 14.0f), t = (int)((1.0f - z) * 15.0f), s = k ? 2 : 3;
            for (i = 0; i < s; i++) {
                PB(zx + i, zy, C565(255, 255, 255), t);
                PB(zx + i, zy + s - 1, C565(255, 255, 255), t);
                PB(zx + s - 1 - i, zy + i, C565(255, 255, 255), t);
            }
        }
    }
    if (!set.clean)
        draw_info_panel(r);
    if (ui.help)
        card_help(r);
    else if (ui.overlay == OV_KNOBS)
        card_knobs(r);
    else if (ui.overlay == OV_BANNER)
        card_banner(r);
    else if (ui.scale_view)
        card_scale(r);
}

/* ------------------------------------------------------ the words over it --- */
static uint16_t c_text, c_dim, c_acc, c_acc2;

static void txt_center(int y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text((240 - text_w(f, s)) / 2, y, f, s, c);
}

static void chord_text(char *b)
{
    int mood = playing ? eng_stat.mood : set.mood, root = playing ? eng_stat.root : set.root;
    int ci = playing && eng_stat.chord >= 0 ? eng_stat.chord : 0;
    const chord_t *c = &MOODS[mood].prog[ci % MOODS[mood].n];
    b[0] = 0;
    text_cat(b, note_in_key(root + c->deg, root, MOODS[mood].scale));
    text_cat(b, QUALS[c->q].suffix);
}

static void words(void)
{
    char t[40];
    const room_t *r = &ROOMS[theme_now()];
    int i;
    c_text = C565(250, 244, 232);
    c_dim = C565(170, 164, 156);
    c_acc = r->accent;
    c_acc2 = r->accent2;
    if (ui.help) {
        static const char *const H[][2] = {
            {"SELECT", "MOOD"}, {"ALGORITHM", "DRUM PATTERN"}, {"PRESET", "WINDOW VIEW"},
            {"K1-K4", "TEMPO DRUM BASS KEYS"}, {"ENV", "GROOVE PAGE"}, {"FX", "TAPE PAGE"},
            {"KEYS  OCT-/+", "SET THE KEY"}, {"PLAY", "START / STOP"}, {"REC", "TAP TEMPO"}, {"SEQ", "DRUMS SIT OUT"},
            {"ARP  LFO", "KEYS / BASS STYLE"}, {"SEL", "SCALE MAP"}, {"EDIT  GLO", "VIEW / THE CAT"},
        };
        txt_center(10, &FONT_B, "NOOK  HELP", c_acc);
        for (i = 0; i < 13; i++) {
            cv_text(16, 31 + i * 15, &FONT_XS, H[i][0], c_acc2);
            cv_text(104, 31 + i * 15, &FONT_XS, H[i][1], c_text);
        }
        return;
    }
    if (!set.clean) {
        /* the top bar: the mood, the tempo */
        cv_text(16, 3, &FONT_B, MOODS[set.mood].name, c_text);
        if (!playing && ((int)(ui.t * 1.2f) & 1)) {
            t[0] = 0;
            text_cat(t, "PRESS PLAY");
        } else {
            int_text(set.bpm, t);
            text_cat(t, " BPM");
        }
        cv_text(232 - text_w(&FONT_XS, t), 4, &FONT_XS, t, playing ? c_dim : c_acc);
        /* the panel: the chord, the key and scale, the drums */
        chord_text(t);
        cv_text(84, 176, &FONT_M, t, c_acc);
        t[0] = 0;
        text_cat(t, KEYNAME(set.root));
        cv_text(84, 201, &FONT_B, t, c_text);
        cv_text(86 + text_w(&FONT_B, t) + 2, 202, &FONT_XS, SCALES[MOODS[set.mood].scale].name, c_dim);
        two_digits(set.pattern + 1, t);
        text_cat(t, " ");
        text_cat(t, PATTERNS[set.pattern].name);
        if (breakdown) {
            t[0] = 0;
            text_cat(t, "DRUMS OUT");
        }
        cv_text(100, 219, &FONT_XS, t, breakdown ? c_dim : c_text);
    }
    if (ui.overlay == OV_KNOBS) {
        t[0] = 0;
        text_cat(t, PAGE_NAME[set.page]);
        cv_text(36, 33, &FONT_B, t, c_acc);
        cv_text(196 - text_w(&FONT_XS, set.page == PG_MAIN ? "HOME" : set.page == PG_GROOVE ? "ENV" : "FX"), 34,
                &FONT_XS, set.page == PG_MAIN ? "HOME" : set.page == PG_GROOVE ? "ENV" : "FX", c_dim);
        for (i = 0; i < 4; i++) {
            int y = 52 + i * 22;
            t[0] = 'K';
            t[1] = (char)('1' + i);
            t[2] = 0;
            cv_text(36, y, &FONT_XS, t, c_dim);
            cv_text(54, y, &FONT_XS, knob_label(set.page, i), i == ui.knob_hi ? c_text : c_dim);
            knob_value_text(set.page, i, t);
            cv_text(204 - text_w(&FONT_XS, t), y, &FONT_XS, t, i == ui.knob_hi ? c_acc : c_dim);
        }
    } else if (ui.overlay == OV_BANNER) {
        txt_center(48, &FONT_XS, ui.ban[0], c_dim);
        txt_center(62, &FONT_M, ui.ban[1], c_acc);
        txt_center(90, &FONT_XS, ui.ban[2], c_text);
    } else if (ui.scale_view) {
        const scale_t *s = &SCALES[MOODS[set.mood].scale];
        t[0] = 0;
        text_cat(t, KEYNAME(set.root));
        text_cat(t, " ");
        text_cat(t, s->name);
        cv_text(18, 38, &FONT_B, t, c_acc);
        t[0] = 0;
        for (i = 0; i < s->n; i++) {
            text_cat(t, KEYNAME(set.root + s->iv[i]));
            if (i < s->n - 1)
                text_cat(t, " ");
        }
        cv_text(222 - text_w(&FONT_XS, t), 40, &FONT_XS, t, c_text);
        cv_text(63, 124, &FONT_XS, "3", c_dim);
        cv_text(91, 124, &FONT_XS, "5", c_dim);
        cv_text(119, 124, &FONT_XS, "7", c_dim);
        cv_text(147, 124, &FONT_XS, "9", c_dim);
        cv_text(188, 124, &FONT_XS, "12", c_dim);
    }
    if (plat_ms() < ui.msg_until && ui.overlay != OV_BANNER && !ui.help) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, "  ");
            text_cat(t, ui.msg[1]);
        }
        {
            int w = text_w(&FONT_XS, t) + 16, x = (240 - w) / 2;
            cv_rect(x, 140, w, 18, r->panel);
            cv_text(x + 8, 141, &FONT_XS, t, c_text);
        }
    }
}

static void splash_words(void)
{
    txt_center(66, &FONT_L, APP_NAME, APP_SPLASH_RGB);
    txt_center(100, &FONT_XS, APP_TAGLINE, C565(240, 230, 220));
}

/* the picture to the screen: two bands of 240 x 120, each pixel doubled, the words over each */
static void present(void (*over)(void))
{
    int band;
    for (band = 0; band < 2; band++) {
        int ly, lx;
        cv_begin(240, 120, 0);
        for (ly = 0; ly < 60; ly++) {
            const uint16_t *src = &lofb[(band * 60 + ly) * LW];
            uint16_t *d0 = &cv_px[(uint32_t)ly * 2u * 240u], *d1 = d0 + 240;
            for (lx = 0; lx < LW; lx++) {
                uint16_t c = swap16(src[lx]);
                d0[2 * lx] = d0[2 * lx + 1] = c;
                d1[2 * lx] = d1[2 * lx + 1] = c;
            }
        }
        cv_oy = -120 * band;             /* the words in screen rows: the band shows its part */
        cv_y0 = 0;
        cv_y1 = 120;
        over();
        cv_oy = 0;
        cv_blit(0, (uint32_t)(band * 120));
    }
}

void app_splash(void)
{
    knobs_init();
    ui.t = 3.0f;
    scene_draw();
    panel(26, 30, 68, 30, ROOMS[theme_now()].panel, ROOMS[theme_now()].accent);
    present(splash_words);
}

void app_ui_init(void)
{
    knobs_init();
    ui.step = -1;
    ui.knob_hi = -1;
    ui.kicks = eng_stat.kicks;
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    input();
    if (ui.overlay && now >= ui.overlay_until)
        ui.overlay = OV_NONE;
    if (now - ui.frame_at >= 40u) {
        float dt = ui.frame_at ? (float)(now - ui.frame_at) * 0.001f : 0.04f;
        if (dt > 0.1f)
            dt = 0.1f;
        ui.frame_at = now;
        scene_step(dt);
        scene_draw();
        present(words);
    }
    ann_tick(now);
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "mood")) *got = set.mood;
    else if (!strcmp(what, "pattern")) *got = set.pattern;
    else if (!strcmp(what, "theme")) *got = set.theme;
    else if (!strcmp(what, "root")) *got = set.root;
    else if (!strcmp(what, "bpm")) *got = set.bpm;
    else if (!strcmp(what, "page")) *got = set.page;
    else if (!strcmp(what, "playing")) *got = playing;
    else if (!strcmp(what, "breakdown")) *got = breakdown;
    else if (!strcmp(what, "dirty")) *got = set_dirty;
    else if (!strcmp(what, "kstyle")) *got = set.kstyle;
    else if (!strcmp(what, "bstyle")) *got = set.bstyle;
    else if (!strcmp(what, "cat")) *got = set.cat;
    else if (!strcmp(what, "drums")) *got = set.drums;
    else if (!strcmp(what, "reverb")) *got = set.reverb;
    else if (!strcmp(what, "bar")) *got = (int)eng_stat.bar;
    else if (!strcmp(what, "step")) *got = eng_stat.step;
    else if (!strcmp(what, "eng_mood")) *got = eng_stat.mood;
    else if (!strcmp(what, "eng_root")) *got = eng_stat.root;
    else if (!strcmp(what, "kicks")) *got = (int)eng_stat.kicks;
    else if (!strcmp(what, "keyhits")) *got = (int)eng_stat.keyhits;
    else if (!strcmp(what, "bassnotes")) *got = (int)eng_stat.bassnotes;
    else if (!strcmp(what, "scale_view")) *got = ui.scale_view;
    else if (!strcmp(what, "help")) *got = ui.help;
    else return 0;
    return 1;
}
#endif
