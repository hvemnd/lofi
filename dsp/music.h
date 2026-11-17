/* SPDX-License-Identifier: MIT
 * NOOK: lo-fi jam companion for the M-VAVE FM-1. */
/* The music: 12 moods (key, scale, tempo, a chord loop, a keys sound and a feel), 20 drum patterns and their kits,
 * the scales the guitarist plays over, the chord qualities. Shared by the engine (dsp/lofi.c) and the UI (app.c). */
#pragma once
#include <stdint.h>

/* ---------------------------------------------------------------- scales --- */
enum { SC_MINPENT, SC_BLUES, SC_DORIAN, SC_MAJPENT, SC_MIXO, NSCALE };
typedef struct {
    const char *name;                    /* on the screen */
    uint8_t n;
    int8_t iv[7];                        /* semitones above the root */
} scale_t;
static const scale_t SCALES[NSCALE] = {
    {"MINOR PENT", 5, {0, 3, 5, 7, 10}},
    {"BLUES", 6, {0, 3, 5, 6, 7, 10}},
    {"DORIAN", 7, {0, 2, 3, 5, 7, 9, 10}},
    {"MAJOR PENT", 5, {0, 2, 4, 7, 9}},
    {"MIXOLYDIAN", 7, {0, 2, 4, 5, 7, 9, 10}},
};

/* --------------------------------------------------------- chord qualities --- */
enum { Q_M7, Q_M9, Q_M11, Q_MAJ7, Q_MAJ9, Q_DOM7, Q_DOM9, Q_DOM13, Q_7S9, Q_SUS9, Q_M6, Q_69, NQUAL };
typedef struct {
    const char *suffix;                  /* after the root's name: "m9" */
    uint8_t n;
    int8_t iv[6];                        /* above the root (the root itself is the bass's) */
} quality_t;
static const quality_t QUALS[NQUAL] = {
    {"m7", 4, {0, 3, 7, 10}},
    {"m9", 5, {0, 3, 7, 10, 14}},
    {"m11", 5, {0, 3, 10, 14, 17}},
    {"maj7", 4, {0, 4, 7, 11}},
    {"maj9", 5, {0, 4, 7, 11, 14}},
    {"7", 4, {0, 4, 7, 10}},
    {"9", 5, {0, 4, 7, 10, 14}},
    {"13", 5, {0, 4, 10, 14, 21}},
    {"7#9", 4, {0, 4, 10, 15}},
    {"9sus", 5, {0, 5, 7, 10, 14}},
    {"m6", 4, {0, 3, 7, 9}},
    {"6/9", 5, {0, 4, 7, 9, 14}},
};

/* ---------------------------------------------------------- keys sounds --- */
enum { KS_RHODES, KS_FELT, KS_WURLI, KS_DREAM, NKSOUND };
static const char *const KSOUND_NAME[NKSOUND] = {"RHODES", "FELT PIANO", "WURLI", "DREAM EP"};

/* ------------------------------------------------------------ the feel --- */
enum { KST_AUTO, KST_PADS, KST_STRUM, KST_COMP, KST_ARP, KST_SPARSE, NKSTYLE };
static const char *const KSTYLE_NAME[NKSTYLE] = {"AUTO", "PADS", "STRUM", "COMP", "ARP", "SPARSE"};
enum { BST_AUTO, BST_HOLD, BST_BOUNCE, BST_WALK, BST_OFF, NBSTYLE };
static const char *const BSTYLE_NAME[NBSTYLE] = {"AUTO", "HOLD", "BOUNCE", "WALK", "OFF"};

/* ----------------------------------------------------------- the themes --- */
enum { TH_AUTO, TH_RAIN, TH_SUNSET, TH_SNOW, TH_NEON, TH_BEACH, TH_AUTUMN, TH_SAKURA, TH_ORBIT, NTHEME };
static const char *const THEME_NAME[NTHEME] = {"AUTO", "RAIN CITY", "SUNSET HILLS", "SNOWY PINES", "NEON STREET",
                                               "MOON BEACH", "AUTUMN PARK", "CHERRY HILL", "ORBIT"};
/* the sound outside each window (the ambience knob's) */
enum { AMB_RAIN, AMB_CRICKETS, AMB_WIND, AMB_WAVES, AMB_HUM, NAMB };
static const uint8_t THEME_AMB[NTHEME] = {AMB_RAIN, AMB_RAIN, AMB_CRICKETS, AMB_WIND, AMB_RAIN,
                                          AMB_WAVES, AMB_WIND, AMB_CRICKETS, AMB_HUM};
static const char *const AMB_NAME[NAMB] = {"RAIN", "CRICKETS", "WIND", "WAVES", "SPACE HUM"};

/* ------------------------------------------------------------ the moods --- */
typedef struct {
    int8_t deg;                          /* the chord's root, semitones above the key */
    uint8_t q;                           /* Q_* */
} chord_t;
typedef struct {
    const char *name;
    uint8_t root;                        /* the key's pitch class (0 = C) */
    uint8_t scale;                       /* SC_*: what the guitar plays over it */
    uint8_t bpm;
    uint8_t ksound, kstyle, bstyle, theme;
    uint8_t n;                           /* chords in the loop, one a bar */
    chord_t prog[8];
} mood_t;
#define NMOOD 12
static const mood_t MOODS[NMOOD] = {
    {"RAINY CAFE", 9, SC_MINPENT, 78, KS_FELT, KST_COMP, BST_BOUNCE, TH_RAIN, 4,
     {{0, Q_M9}, {5, Q_M9}, {8, Q_MAJ7}, {7, Q_M7}}},
    {"LATE NIGHT", 2, SC_DORIAN, 72, KS_RHODES, KST_PADS, BST_HOLD, TH_NEON, 4,
     {{0, Q_M9}, {5, Q_DOM13}, {0, Q_M9}, {10, Q_MAJ9}}},
    {"SUNDAY BLUES", 4, SC_BLUES, 84, KS_WURLI, KST_COMP, BST_WALK, TH_SUNSET, 8,
     {{0, Q_DOM9}, {5, Q_DOM13}, {0, Q_DOM9}, {0, Q_DOM9}, {5, Q_DOM13}, {5, Q_DOM13}, {0, Q_DOM9}, {7, Q_7S9}}},
    {"MOONLIT", 4, SC_MINPENT, 70, KS_DREAM, KST_SPARSE, BST_HOLD, TH_BEACH, 4,
     {{0, Q_M9}, {8, Q_MAJ9}, {5, Q_M9}, {7, Q_M7}}},
    {"CITY LIGHTS", 9, SC_DORIAN, 88, KS_RHODES, KST_COMP, BST_BOUNCE, TH_NEON, 4,
     {{0, Q_M9}, {2, Q_M7}, {3, Q_MAJ7}, {5, Q_DOM9}}},
    {"SAKURA", 2, SC_MAJPENT, 82, KS_FELT, KST_ARP, BST_BOUNCE, TH_SAKURA, 4,
     {{0, Q_MAJ9}, {9, Q_M9}, {5, Q_MAJ9}, {7, Q_SUS9}}},
    {"SLOW BURN", 9, SC_BLUES, 66, KS_WURLI, KST_STRUM, BST_HOLD, TH_SUNSET, 4,
     {{0, Q_M9}, {5, Q_DOM9}, {0, Q_M9}, {7, Q_7S9}}},
    {"COFFEE STEAM", 0, SC_MAJPENT, 86, KS_RHODES, KST_COMP, BST_WALK, TH_AUTUMN, 4,
     {{5, Q_MAJ7}, {4, Q_M7}, {2, Q_M9}, {0, Q_MAJ9}}},
    {"STARGAZER", 11, SC_MINPENT, 74, KS_DREAM, KST_ARP, BST_HOLD, TH_ORBIT, 4,
     {{0, Q_M9}, {8, Q_MAJ7}, {5, Q_M9}, {7, Q_M7}}},
    {"SNOWFALL", 7, SC_MIXO, 76, KS_FELT, KST_SPARSE, BST_HOLD, TH_SNOW, 4,
     {{0, Q_DOM9}, {10, Q_MAJ9}, {5, Q_MAJ9}, {7, Q_M9}}},
    {"HAZE", 6, SC_MINPENT, 80, KS_DREAM, KST_PADS, BST_BOUNCE, TH_RAIN, 4,
     {{0, Q_M9}, {8, Q_MAJ7}, {5, Q_M9}, {7, Q_M7}}},
    {"ROOFTOP", 4, SC_DORIAN, 92, KS_RHODES, KST_COMP, BST_BOUNCE, TH_SUNSET, 4,
     {{0, Q_M9}, {5, Q_DOM9}, {3, Q_MAJ7}, {2, Q_M7}}},
};

/* ------------------------------------------------------- drum patterns --- */
/* 16 sixteenths a bar, one string per drum: 'X' accent, 'x' a hit, 'o' a ghost, '.' nothing.
 * K kick, S snare (or clap: the kit's), H closed hat, O open hat, R rim / side stick, P shaker */
enum { KIT_DUSTY, KIT_WARM, KIT_CRISP, KIT_SOFT, NKIT };
typedef struct {
    const char *name;
    const char *k, *s, *h, *o, *r, *p;
    uint8_t kit;
    uint8_t swing_min;                   /* a floor under the swing knob, % (a shuffle is a shuffle) */
    int8_t lazy_ms;                      /* the snare (and rim) laid back by this much: the drunk feel */
} pattern_t;
#define NPATTERN 20
static const pattern_t PATTERNS[NPATTERN] = {
    {"BOOM BAP",     "X.....x...X..x..", "....X.......X...", "x.x.x.x.x.x.x.x.", "................",
     "................", "................", KIT_DUSTY, 0, 0},
    {"DILLA SWAY",   "X..x......X.x...", "....X..o....X..o", "xoxoxoxoxoxoxoxo", "................",
     "................", "................", KIT_DUSTY, 35, 18},
    {"LAZY SUNDAY",  "X.......X.x.....", "....X.......X...", "x...x...x...x...", "..............x.",
     "................", "..x...x...x...x.", KIT_WARM, 0, 8},
    {"HALF TIME",    "X.........x.....", "........X.......", "x.x.x.x.x.x.x.x.", "................",
     "....o.......o...", "................", KIT_WARM, 0, 0},
    {"RIM SHOT",     "X.....x.X.......", "................", "x.xxx.x.x.xxx.x.", "................",
     "....X..o....X...", "................", KIT_CRISP, 0, 0},
    {"BRUSHES",      "X.......X.......", "....o.......o...", "..x...x...x...x.", "................",
     "................", "xoxoxoxoxoxoxoxo", KIT_SOFT, 30, 0},
    {"BOSSA",        "X..xX..xX..xX..x", "................", "xxxxxxxxxxxxxxxx", "................",
     "X..X..X...X..X..", "................", KIT_SOFT, 0, 0},
    {"SHUFFLE",      "X.....x.X.....x.", "....X.......X...", "x.x.x.x.x.x.x.x.", "...............x",
     "................", "................", KIT_WARM, 55, 0},
    {"TRAP LITE",    "X......X..X.....", "........X.......", "x.x.xxx.x.x.x.xx", "................",
     "................", "................", KIT_CRISP, 0, 0},
    {"DUSTY 2-STEP", "X.....x...x.....", "....X.......X...", "..x...x...x...x.", "................",
     "................", "x...x...x...x...", KIT_DUSTY, 15, 0},
    {"KNOCK",        "X..x..x...X.....", "....X.......X.o.", "x.x.x.x.x.x.x...", "..............x.",
     "................", "................", KIT_DUSTY, 20, 6},
    {"SIDE STICK",   "X.......X.x.....", "................", "xoxoxoxoxoxoxoxo", "................",
     "....X.......X...", "................", KIT_SOFT, 25, 0},
    {"HEARTBEAT",    "X.x.........X.x.", "............X...", "x.......x.......", "................",
     "....o...........", "................", KIT_WARM, 0, 0},
    {"STROLL",       "X.......X.......", "....X.......X...", "x.xox.xox.xox.xo", "................",
     "................", "................", KIT_SOFT, 40, 0},
    {"SKIP HOP",     "X..x..x..x..X...", "....X.......X...", "x.x.x.x.x.x.x.x.", "................",
     "................", "................", KIT_CRISP, 10, 0},
    {"TAPE LOOP",    "X.....X...X..x..", "....X..o.o..X...", "x.x.x.x.x.x.x.x.", "............x...",
     "................", "................", KIT_DUSTY, 20, 4},
    {"BREAKBEAT",    "X.X.......XX....", "....X..o.o..X..o", "x.x.x.x.x.x.x.x.", "................",
     "................", "................", KIT_CRISP, 10, 0},
    {"NODDING",      "X.......Xx......", "....X.......X...", "x.x.x.x.x.x.x.x.", "................",
     "................", "..x...x...x...x.", KIT_WARM, 15, 10},
    {"STEPPER",      "X...X...X...X...", "....X.......X...", "x...x...x...x...", "..x...x...x...x.",
     "................", "................", KIT_SOFT, 0, 0},
    {"MINIMAL",      "X.........x.....", "................", "x.x.x.x.x.x.x.x.", "................",
     "............o...", "................", KIT_SOFT, 0, 0},
};

/* a note's name, sharps or flats as the key likes them */
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const NOTE_FLAT[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
/* flats when the scale's parent major key is a flat one (F, Bb, Eb, Ab, Db) */
static inline int key_flats(int root, int scale)
{
    static const int8_t TO_MAJOR[NSCALE] = {3, 3, 10, 0, 5};   /* minor pent, blues: the relative major; dorian a tone
                                                              * down; major pent itself; mixolydian a fourth up */
    int maj = (root + TO_MAJOR[scale]) % 12;
    return maj == 5 || maj == 10 || maj == 3 || maj == 8 || maj == 1;
}
static inline const char *note_in_key(int pc, int root, int scale)
{
    pc = ((pc % 12) + 12) % 12;
    return key_flats(root, scale) ? NOTE_FLAT[pc] : NOTE_NAME[pc];
}
