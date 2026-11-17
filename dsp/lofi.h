/* SPDX-License-Identifier: MIT
 * NOOK: lo-fi jam companion for the M-VAVE FM-1. */
/* The sound engine: a generative lo-fi band. Drums (kick, snare / clap, hats, rim, shaker) on the 20 patterns,
 * a bass and an electric piano that make up their parts each bar from the mood's chord loop, and the tape:
 * wow and flutter, a warm low-pass, vinyl crackle, a little reverb, sidechain, and the sound outside the window.
 *
 * The main loop writes eng_ctl (plain fields, each one a word: no lock); the audio interrupt reads it once a
 * block, and takes a new mood, key or pattern at the next bar line. eng_stat is the other way: what the band is
 * doing, for the screen. */
#pragma once
#include <stdint.h>

#define ENG_SR 44100.0f

typedef struct {
    int32_t mood;                        /* MOODS[] */
    int32_t root;                        /* the key, pitch class 0..11 */
    int32_t pattern;                     /* PATTERNS[] */
    int32_t kstyle, bstyle;              /* KST_*, BST_* (AUTO: the mood's) */
    int32_t playing;                     /* 0 stopped (the band lets its notes ring out), 1 playing */
    int32_t breakdown;                   /* 1: the drums sit out (from the next bar) */
    int32_t amb;                         /* AMB_*: the sound outside */
    int32_t midi_out;                    /* notes and clock out over USB */
    float bpm;
    float swing, variation, density;     /* 0..1 */
    float drums, bass, keys, ambience;   /* levels 0..1 */
    float crackle, wow, tone, reverb;    /* 0..1 */
} eng_ctl_t;

typedef struct {
    int32_t step;                        /* the sixteenth playing (0..15), -1 stopped */
    uint32_t bar;                        /* bars since PLAY */
    int32_t chord;                       /* index in the mood's loop */
    int32_t chord_root;                  /* the chord's root, pitch class (-1 none yet) */
    int32_t mood, root, pattern;         /* what is playing now (changes land on a bar line) */
    uint32_t kicks, snares, hats, beats, keyhits, bassnotes;   /* counts, for the animations */
    int32_t bass_note;                   /* MIDI, -1 none */
    int32_t top_note;                    /* the piano's highest note sounding, -1 none */
    uint32_t keys_mask;                  /* pitch classes the piano is holding (bit pc) */
    float level;                         /* output, 0..1, smoothed */
    float duck;                          /* the sidechain's dip now, 0..1 */
    uint32_t xfills;                     /* fills played */
} eng_stat_t;

extern volatile eng_ctl_t eng_ctl;
extern volatile eng_stat_t eng_stat;

void eng_init(void);
void eng_render(int32_t *lr, uint32_t n, float gain);
void eng_panic(void);
void eng_blip(int up);                   /* a little UI sound on a page change (soft) */
