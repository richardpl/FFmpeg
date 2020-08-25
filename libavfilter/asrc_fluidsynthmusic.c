/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <float.h>
#include <stdio.h>
#include <string.h>
#include <fluidsynth.h>
#include <stdlib.h>
#include <unistd.h>

#include "libavutil/avassert.h"
#include "libavutil/channel_layout.h"
#include "libavutil/eval.h"
#include "libavutil/opt.h"
#include "libavutil/lfg.h"
#include "libavutil/random_seed.h"
#include "libavutil/common.h"
#include "audio.h"
#include "avfilter.h"
#include "internal.h"
#include "notedef.h"

typedef struct FluidsynthmusicContext
{
    const AVClass *class;
    int64_t duration;
    int nb_samples;
    int sample_rate;
    int64_t pts;
    int infinite;

    fluid_settings_t *settings;
    fluid_synth_t *synth;
    fluid_sequencer_t *sequencer;
    fluid_seq_id_t synth_destination;
    fluid_seq_id_t client_destination;
    unsigned int beat_dur;
    unsigned int beats_pm;
    unsigned int time_marker;
    char *sfont;
    int velocity;
    int percussion_velocity;
    double changerate;

    int *riffs;
    int numriffs;
    int last_note;
    int framecount;
    char *instrument;
    int *track;
    char *track_name;
    int numbars;
    int64_t seed;
    AVLFG r;

    char *axiom;
    char *rule1;
    char *rule2;
    char *prevgen;
    char *nextgen;
    lsys *system;
    int generations;
    int lstate;
    int max;

    int ca_cells[32];
    int ca_nextgen[32];
    int *ca_neighbours;
    int *ca_8keys[8];
    int *ca_ruleset;
    int *note_map;
    int *scale;
    int ca_boundary;
    int ca_rule;
    int ca_ruletype;
    int height;
    int ca_nsize;
    void (*ca_generate)(int *curr, int *next, int *keys, int *nbor, int *ruleset, int size, int height, AVLFG *rand);
    char *scale_name;
    int last_bass_note;
    int last_lead_note;
    void (*schedule_pattern)(void *t);
    int algorithm;
    void (*ca_bass)(void *t);
    void (*ca_chords)(void *t);
    void (*ca_lead)(void *t);
    int ca_bass_name;
    int ca_chords_name;
    int ca_lead_name;
    char *chords_instr;
    char *bass_instr;
    char *lead_instr;

    int *p_instr;
    int *p_beats;
    int p_maxres;
    int p_density;
    int p_barstate;
    int p_algorithm;
} FluidsynthmusicContext;

#define OFFSET(x) offsetof(FluidsynthmusicContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

enum boundary {
    INFINITE,
    CYCLIC
};

enum bass_algorithm {
    B_LOWEST_NOTES,
    B_LOWER_EIGHTH
};

enum chord_algorithm {
    C_EIGHTH,
    C_WHOLE
};

enum lead_algortihm {
    L_UPPER_EIGHTH,
    L_UPPER_WHOLE,
    L_LOWER_EIGHTH,
    L_LOWER_WHOLE
};

enum rhythm_algorithm {
    PADRIDDLE,
    TOGGLE,
    ALTERNATE,
    TOGPAD,
    ALTPAD,
    TOGALT
};

enum algorithm {
    RIFFS,
    LSYSTEM,
    CA,
    RHYTHM
};

enum algo_channel{
    RIFFNL,
    CA_BASS,
    CA_LEAD,
    CA_CHORDS,
    PERCUSSION = 9
};

static const AVOption fluidsynthmusic_options[] = {
    {"velocity",         "set velocity of key press",                    OFFSET(velocity),            AV_OPT_TYPE_INT,     {.i64 = 80},    0, 127,       FLAGS},
    {"v",                "set velocity of key press",                    OFFSET(velocity),            AV_OPT_TYPE_INT,     {.i64 = 80},    0, 127,       FLAGS},
    {"p_velocity",       "set percussion velocity",                      OFFSET(percussion_velocity), AV_OPT_TYPE_INT,     {.i64 = 80},    0, 127,       FLAGS},
    {"sample_rate",      "set sample rate",                              OFFSET(sample_rate),         AV_OPT_TYPE_INT,     {.i64 = 44100}, 1, INT_MAX,   FLAGS},
    {"r",                "set sample rate",                              OFFSET(sample_rate),         AV_OPT_TYPE_INT,     {.i64 = 44100}, 1, INT_MAX,   FLAGS},
    {"duration",         "set duration in seconds",                      OFFSET(duration),            AV_OPT_TYPE_DURATION,{.i64 = 0},     0, INT64_MAX, FLAGS},
    {"d",                "set duration in seconds",                      OFFSET(duration),            AV_OPT_TYPE_DURATION,{.i64 = 0},     0, INT64_MAX, FLAGS},
    {"nb_samples",       "set number of samples per frame",              OFFSET(nb_samples),          AV_OPT_TYPE_INT,     {.i64 = 1024},  0, INT_MAX,   FLAGS},
    {"seed",             "set seed for random number generator",         OFFSET(seed),                AV_OPT_TYPE_INT,     {.i64 = -1},    -1,INT_MAX,   FLAGS},
    {"bpm",              "set beats per minute",                         OFFSET(beats_pm),            AV_OPT_TYPE_INT,     {.i64 = 80},    1, INT_MAX,   FLAGS},
    {"sfont",            "set the path to soundfont file",               OFFSET(sfont), AV_OPT_TYPE_STRING, {.str = "/usr/share/sounds/sf2/FluidR3_GM.sf2"}, 0, 0, FLAGS},
    {"instrument",       "set instrument for riff and L system",         OFFSET(instrument),          AV_OPT_TYPE_STRING,  {.str = "Acoustic-Grand"}, 0, 0, FLAGS},
    {"percussion",       "set percussion track",                         OFFSET(track_name),          AV_OPT_TYPE_STRING,  {.str = "Shuffle"},      0, 0, FLAGS},
    {"numbars",          "set number of bars in riff",                   OFFSET(numbars),             AV_OPT_TYPE_INT,     {.i64 = 2},     2, 8, FLAGS},
    {"axiom",            "set the L system axiom",                       OFFSET(axiom),               AV_OPT_TYPE_STRING,  {.str = "CFppFmmXD"}, 0, 0, FLAGS},
    {"rule1",            "set rule1 of L system",                        OFFSET(rule1),               AV_OPT_TYPE_STRING,  {.str = "XtoFCppppFmmmmXDCmmFpppD"}, 0, 0, FLAGS},
    {"rule2",            "set rule2 of L system",                        OFFSET(rule2),               AV_OPT_TYPE_STRING,  {.str = "FtoCppppFmmmFpppFD"}, 0, 0, FLAGS},
    {"gen",              "set no of generations of L system",            OFFSET(generations),         AV_OPT_TYPE_INT,     {.i64 = 3},     0, INT_MAX,   FLAGS},
    {"ruletype",         "set ruletype of cellular automaton",           OFFSET(ca_ruletype),         AV_OPT_TYPE_INT,     {.i64  =  31},  0, INT_MAX,   FLAGS},
    {"rule",             "set rule of cellular automaton",               OFFSET(ca_rule),             AV_OPT_TYPE_INT,     {.i64 = 367921},0, INT_MAX,   FLAGS},
    {"height",           "set height for mapping of scale ",             OFFSET(height),              AV_OPT_TYPE_INT,     {.i64 = 20}, 10,25, FLAGS},
    {"bass_instr",       "set bass instrument of cellular automaton",    OFFSET(bass_instr),          AV_OPT_TYPE_STRING,  {.str = "Acoustic-Grand"}, 0, 0, FLAGS},
    {"chord_instr",      "set chords instrument of cellular automaton",  OFFSET(chords_instr),        AV_OPT_TYPE_STRING,  {.str = "Acoustic-Grand"}, 0, 0, FLAGS},
    {"lead_instr",       "set lead instrument of cellular automaton",    OFFSET(lead_instr),          AV_OPT_TYPE_STRING,  {.str = "Acoustic-Grand"}, 0, 0, FLAGS},
    {"scale",            "set scale for cellular automaton and L system",OFFSET(scale_name),          AV_OPT_TYPE_STRING,  {.str = "C_major"}, 0, 0, FLAGS},
    {"bass",             "set bass algorithm for cellular automaton",    OFFSET(ca_bass_name),        AV_OPT_TYPE_INT,     {.i64 = B_LOWEST_NOTES}, 0, 1, FLAGS, "bass"},
    {"lowest_notes",      0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = B_LOWEST_NOTES}, 0, 0, FLAGS, "bass"},
    {"lower_eighth",      0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = B_LOWER_EIGHTH}, 0, 0, FLAGS, "bass"},
    {"chords",           "set chords algorithm for cellular automaton",  OFFSET(ca_chords_name),      AV_OPT_TYPE_INT,     {.i64 = C_WHOLE},        0, 1, FLAGS, "chords"},
    {"eighth",            0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = C_EIGHTH},       0, 0, FLAGS, "chords"},
    {"whole",             0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = C_WHOLE},        0, 0, FLAGS, "chords"},
    {"lead",             "set lead algorithm for cellular automaton",    OFFSET(ca_lead_name),        AV_OPT_TYPE_INT,     {.i64 = L_UPPER_WHOLE},  0, 3, FLAGS, "lead"},
    {"upper_eighth",      0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = L_UPPER_EIGHTH}, 0, 0, FLAGS, "lead"},
    {"upper_whole",       0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = L_UPPER_WHOLE},  0, 0, FLAGS, "lead"},
    {"lower_eighth",      0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = L_LOWER_EIGHTH}, 0, 0, FLAGS, "lead"},
    {"lower_whole",       0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = L_LOWER_WHOLE},  0, 0, FLAGS, "lead"},
    {"boundary",         "set boundary type for cellular automaton",     OFFSET(ca_boundary),         AV_OPT_TYPE_INT,     {.i64 = CYCLIC},         0, 1, FLAGS, "boundary"},
    {"infinite",          0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64= INFINITE},        0, 0, FLAGS, "boundary"},
    {"cyclic",            0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64= CYCLIC},          0, 0, FLAGS, "boundary"},
    {"maxres",           "set maximum resolution of rhythm algo",        OFFSET(p_maxres),            AV_OPT_TYPE_INT,     {.i64 = 16},             4, 64, FLAGS, "maxres"},
    {"density",          "set density of notes in rhythm algo",          OFFSET(p_density),           AV_OPT_TYPE_INT,     {.i64 = 75},             1, 100, FLAGS},
    {"r_algo",           "set the type for rhythm algorithm",            OFFSET(p_algorithm),         AV_OPT_TYPE_INT,     {.i64 = ALTPAD},         0, TOGALT, FLAGS, "r_algo"},
    {"padriddle",         0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = PADRIDDLE},      0, 0, FLAGS, "r_algo"},
    {"toggle",            0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = TOGGLE},         0, 0, FLAGS, "r_algo"},
    {"alternate",         0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = ALTERNATE},      0, 0, FLAGS, "r_algo"},
    {"togpad",            0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = TOGPAD},         0, 0, FLAGS, "r_algo"},
    {"altpad",            0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = ALTPAD},         0, 0, FLAGS, "r_algo"},
    {"togalt",            0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = TOGALT},         0, 0, FLAGS, "r_algo"},
    {"algo",             "set algorithm name",                           OFFSET(algorithm),           AV_OPT_TYPE_INT,     {.i64 = CA},             0, RHYTHM, FLAGS, "algo"},
    {"riffs",             0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = RIFFS},          0, 0, FLAGS, "algo"},
    {"lsystem",           0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = LSYSTEM},        0, 0, FLAGS, "algo"},
    {"ca",                0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = CA},             0, 0, FLAGS, "algo"},
    {"cellular_automaton",0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = CA},             0, 0, FLAGS, "algo"},
    {"rhythm",            0,                                             0,                           AV_OPT_TYPE_CONST,   {.i64 = RHYTHM},          0, 0, FLAGS, "algo"},
    {NULL}
};

AVFILTER_DEFINE_CLASS(fluidsynthmusic);

static void instrument_select(int prog_no, unsigned int ticks, int channel, FluidsynthmusicContext *s)
{
    fluid_event_t *ev = new_fluid_event();

    fluid_event_set_source(ev, -1);
    fluid_event_set_dest(ev, s->synth_destination);
    fluid_event_program_change(ev, channel, prog_no);
    fluid_sequencer_send_at(s->sequencer, ev, ticks, 1);
    delete_fluid_event(ev);
}

/* schedule a note on message */
static void schedule_noteon(int chan, short key, unsigned int ticks, int velocity, FluidsynthmusicContext *s)
{
    fluid_event_t *ev = new_fluid_event();

    fluid_event_set_source(ev, -1);
    fluid_event_set_dest(ev, s->synth_destination);
    fluid_event_noteon(ev, chan, key, velocity);
    fluid_sequencer_send_at(s->sequencer, ev, ticks, 1);
    delete_fluid_event(ev);
}

/* schedule a note off message */
static void schedule_noteoff(int chan, short key, unsigned int ticks, FluidsynthmusicContext *s)
{
    fluid_event_t *ev = new_fluid_event();

    fluid_event_set_source(ev, -1);
    fluid_event_set_dest(ev, s->synth_destination);
    fluid_event_noteoff(ev, chan, key);
    fluid_sequencer_send_at(s->sequencer, ev, ticks, 1);
    delete_fluid_event(ev);
}

/* schedule a timer event to trigger the callback */
static void schedule_timer_event(FluidsynthmusicContext *s)
{
    fluid_event_t *ev = new_fluid_event();

    fluid_event_set_source(ev, -1);
    fluid_event_set_dest(ev, s->client_destination);
    fluid_event_timer(ev, s);
    fluid_sequencer_send_at(s->sequencer, ev, s->time_marker, 1);
    delete_fluid_event(ev);
}

/*---Riffology---*/
/*Determine the closest riff to the previous riff within
  three tries to make the transition between riffs smoother*/
static int pick_riff(FluidsynthmusicContext *s)
{
    int min, dn, riff, bestriff = 0;
    unsigned rand = av_lfg_get(&s->r) / 2;

    min = 999;
    for (int i = 2; i >= 0; i--) {
        riff = rand % s->numriffs;
        if (s->last_note == 0)
            return(riff);
        dn = abs(s->last_note - s->riffs[riff * NPR]);
        if (dn == 0)
            dn = 6;
        if (dn < min) {
            bestriff = riff;
            min = dn;
        }
    }

    return bestriff;
}

/*Determine the energy of the player which will
  affect the number of rests and holding tones*/
static int energy_calc(int i, int numbars)
{
    if (3 * i < numbars)
        return (100 - (90 * i)/numbars);
    else if (3 * i > 2 * numbars)
        return (40 + (90 * i)/numbars);
    return 70;
}

static void play_riff(int riff, int energy, int note_duration, int note_time, FluidsynthmusicContext *s)
{
    int pnd = 0, next;
    short pn = 0 ;
    /*Beat importance values chosen such that off beat values are more likely to be skipped than on beat*/
    int biv[] = {28, 0, 7, 0, 14, 0, 7, 4};
    unsigned rand;

    for (int i = 0; i < NPR; i++) {
        rand = av_lfg_get(&s->r) / 2;
        next = s->riffs[riff * NPR + i];
        if (next != H && next != R && ((energy + biv[i]) < rand % 100))
            next = (rand < RAND_MAX / 2) ? H : R;
        if (next == H) {
            pnd ++;
            continue;
        }

        if (pn != R) {
            schedule_noteon(RIFFNL, pn, note_time, s->velocity, s);
            note_time += pnd*note_duration;
            schedule_noteoff(RIFFNL, pn, note_time, s);
            s->last_note = pn;
        }
        pn = next;
        pnd = 1;
    }

    if (pn != R && pn != H) {
        schedule_noteon(RIFFNL, pn, note_time, s->velocity, s);
        note_time += pnd * note_duration;
        schedule_noteoff(RIFFNL, pn, note_time, s);
        s->last_note = pn;
    }

}

static void play_percussion(FluidsynthmusicContext *s)
{
    int note_time = s->time_marker;

    for (int i = 0; i < s->track[3]; i++)
    {
        /*percussion instruments in channel 10 */
        schedule_noteon(PERCUSSION, s->track[4 * i], note_time, s->percussion_velocity, s);
        schedule_noteon(PERCUSSION, s->track[4 * i + 1], note_time, s->percussion_velocity,s);
        schedule_noteon(PERCUSSION, s->track[4 * i + 2], note_time, s->percussion_velocity,s);
        /*Multiply by 4 as quarter note takes 1 beat, Whole note takes 4 beats and so on*/
        note_time += 4 * s->beat_dur / s->track[3];
        schedule_noteoff(PERCUSSION, s->track[4 * i], note_time, s);
        schedule_noteoff(PERCUSSION, s->track[4 * i + 1], note_time, s);
        schedule_noteoff(PERCUSSION, s->track[4 * i + 2], note_time, s);
    }
}

/*Determine the pattern, tempo (to play as 8th, 16th or 32nd notes) and add the riffs to sequencer
Reference: http://peterlangston.com/Papers/amc.pdf */
static void schedule_riff_pattern(void *t)
{
    FluidsynthmusicContext *s = t;
    int note_time, note_duration, tempo, rpb, energy, riff;
    unsigned rand = av_lfg_get(&s->r) / 2;

    note_time = s->time_marker;
    tempo = 1;

    if (tempo > rand % 3)
        tempo--;
    else if (tempo < rand % 3)
        tempo++;
    tempo = tempo % 3;
    rpb = 1 << tempo;
    note_duration = 4 * s->beat_dur / (NPR * rpb);
    energy = energy_calc(rand % s->numbars, s->numbars);
    for  (int r = 0; r < rpb; r++) {
        riff = pick_riff(s);
        play_riff(riff, energy, note_duration, note_time, s);

    }

    play_percussion(s);
    s->time_marker += 4 * s->beat_dur;
}

/*---Lindenmayer System---*/
/*Schedule 0L system pattern: decode symbols as :
  F -> increase note duration by factor of 2
  X -> rest note
  p -> move up in scale by one note
  m -> move down in scale by one note
  { -> push current state
  } -> set note state to initial value
Reference : https://link.springer.com/chapter/10.1007%2F978-3-540-32003-6_56
 */
static void schedule_0L_pattern(FluidsynthmusicContext *s)
{
    int note_state = s->height / 2, dur_state = 1, sys_state = 0, size = s->height;
    char c;

    for (int i = 0; i < s->generations; i++) {
        int j = 0, length = 0;
        char c;

        while (s->prevgen[j] != '\0') {
            c = s->prevgen[j];

            if (length >= L_MAX_LENGTH)
                break;

            if (c == s->rule1[0]) {
                memcpy(s->nextgen + length, s->rule1 + 3, strlen(s->rule1) - 3);
                length += strlen(s->rule1) - 3;
            }

            else if (c == s->rule2[0]) {
                memcpy(s->nextgen + length, s->rule2 + 3, strlen(s->rule2) - 3);
                length += strlen(s->rule2) - 3;
            }

            else {
                memcpy(s->nextgen+length, s->prevgen+j, 1);
                length +=1;
            }
            j++;
        }
        s->nextgen[L_MAX_LENGTH * 2 - 1] = '\0';
        memcpy(s->prevgen, s->nextgen, strlen(s->nextgen) + 1);
        strcpy(s->nextgen, "");
    }

    for (int i = 0 ; i < strlen(s->prevgen) ; i++) {
        c = s->prevgen[i];
        switch(c) {
            case 'F': dur_state *= 2 ;break;
            case 'p': note_state++; if (note_state >= size) note_state -= size/2; break;
            case 'm': note_state--; if (note_state < 0) note_state += size/2;break;
            case 'C': s->system[sys_state].note = s->note_map[note_state];
                      s->system[sys_state].dur = dur_state; sys_state++; break;
            case 'D': note_state = 0; dur_state = 1; break;
            case 'X': s->system[sys_state].note = R; s->system[sys_state].dur = dur_state;
                      sys_state++; break;
        }
    }

    s->max = sys_state;
}

static void schedule_L_pattern(void *t)
{
    FluidsynthmusicContext *s = t;
    int note_time = s->time_marker, sum, state;

    sum = 0;
    state = s->lstate;
    while (sum < 8 && state < L_MAX_LENGTH && state > 0) {
        sum += s->system[state].dur;
        state++;
    }

    if (state < s->max)
        for (int i = s->lstate; i < state ; i++) {
            if (s->system[i].note == R) {
                note_time += 4 * s->beat_dur * s->system[i].dur / 8;
            }
            else {
                schedule_noteon(RIFFNL, s->system[i].note, note_time, s->velocity, s);
                note_time += 4 * s->beat_dur*s->system[i].dur / 8;
                schedule_noteoff(RIFFNL, s->system[i].note, note_time, s);
            }

        }

    s->lstate += state;
    play_percussion(s);
    s->time_marker += 4*s->beat_dur;
}

/*---Cellular Automaton---*/
static void multiple_notes(int note_time, int start, int length, int on, int *notes, FluidsynthmusicContext *s)
{
    if (on == 1){
        for (int i = start; i < length; i ++)
            schedule_noteon(CA_CHORDS, notes[i], note_time, 2 * s->velocity / 3, s);
    }
    else {
        for (int i = start; i < length; i ++)
            schedule_noteoff(CA_CHORDS, notes[i], note_time, s);
    }

}

static void cyclic_generate (int *curr, int *next, int *keys, int *nbor, int *ruleset, int size, int height, AVLFG *rand)
{
    for (int i = 0; i < 32; i++) {
        int c = 0;

        for (int j = 0; j < size; j++) {
            c +=  curr[(i + nbor[j]+ 32) % 32] << j;
        }
        next[i] = ruleset[c];
    }

    memcpy(curr, next, 32 * sizeof(int));
    memcpy(keys, &curr[16 - height / 2], height * sizeof(int));
}

/*Keep the ratio of 0 and 1 in cell array of cellular automaton same as in rule to simulate infinite boudary */
static void infinite_generate (int *curr, int *next, int *keys, int *nbor, int *ruleset, int size, int height, AVLFG *rand)
{
    float rp = 0.0;

    for (int i = 0; i < (1 << size); i++)
        rp += ruleset[i] * 1.0 / (1 << size);
    for (int i = 0; i < 32; i++){
        int c = 0;

        for (int j = 0; j < size; j++){
            if ((i + nbor[j]) >= size || (i + nbor[j]) < 0){
                float x = av_lfg_get(rand) * 0.5 / INT_MAX;

                if (x > rp){
                    c += 1 << j;
                }
            }
            else
                c +=  curr[i + nbor[j]] << j;
        }

        next[i] = ruleset[c];
    }
    memcpy(curr, next, 32 * sizeof(int));
    memcpy(keys, &curr[16 - height / 2], height * sizeof(int));
}

static void ca_bass_lowest_notes (void *t)
{
    FluidsynthmusicContext *s = t;
    int note_time = s->time_marker;

    for (int j = 0; j < 8; j++) {
        int i = 0;

        while (i < s->height / 3) {
            if (s->ca_8keys[j][i] == 1) {
                s->last_bass_note = i;
                break;
            }
            i++;
        }
        schedule_noteon (CA_BASS, s->note_map[s->last_bass_note % s->height], note_time, 3 * s->velocity / 4, s);
        note_time += 4 * s->beat_dur / 8;
        schedule_noteoff(CA_BASS, s->note_map[s->last_note%s->height], note_time, s);
    }
}

/*Each note obtained is played as a 1/8 note
  Random number obtained is % (2 * i + 1) to increase bias towards upper notes*/
static void ca_bass_lower_eighth (void *t)
{
    FluidsynthmusicContext *s = t;
    int note_time = s->time_marker, note = s->last_bass_note;

    for (int j = 0; j < 8; j++) {
        unsigned max = 0;

        for (int i = FFMAX(0, s->last_bass_note - 3); i < FFMIN(s->last_bass_note + 3, s->height / 2); i++) {
            unsigned rand = (av_lfg_get(&s->r) * s->ca_8keys[j][i]) % (2 * i + 1);

            if (max < rand) {
                max = rand;
                note = i;
            }
        }
        if (max > 0) {
            s->last_bass_note = note;
            schedule_noteon (CA_BASS, s->note_map[s->last_bass_note % s->height], note_time, 2 * s->velocity / 3, s);
            note_time += 4 * s->beat_dur / 8;
            schedule_noteoff(CA_BASS, s->note_map[s->last_note%s->height], note_time, s);
        }
    }
}

static void ca_chords_eighth (void *t)
{
    enum {ON, OFF};
    FluidsynthmusicContext *s = t;
    int note_time = s->time_marker, note = s->last_note, notes[3];

    for (int j = 0; j < 8; j++) {
        unsigned max = 0;

        for (int i = 0; i < s->height; i++) {
            unsigned rand = (av_lfg_get(&s->r) * s->ca_8keys[j][i]) % (2 * i + 1);

            if ((s->ca_8keys[j][i] % s->height) == 1 && (s->ca_8keys[j][i + 2] % s->height) == 1 && (s->ca_8keys[j][i + 4] % s->height) == 1) {
                if (max < rand) {
                    max = rand;
                    note  = i;
                }
            }
        }
        if (max > 0) {
            s->last_note = note;
            for (int i = 0; i < 3; i++)
                notes[i] = s->note_map[(s->last_note + 2 * i) % s->height];
            multiple_notes (note_time, 0, 3, ON, notes, s);
            note_time += 4 * s->beat_dur / 8;
            multiple_notes (note_time, 0, 3, OFF, notes, s);
        }
    }
}

static void ca_chords_whole (void *t)
{
    enum {ON, OFF};
    FluidsynthmusicContext *s = t;
    int note_time = s->time_marker, note[8], notes[3], k;

    for (int j = 0; j < 8; j++) {
        unsigned max = 0;

        note[j] = 0;
        for (int i = 0; i < s->height; i++) {
            unsigned rand = (av_lfg_get(&s->r) * s->ca_8keys[j][i]) % (2 * i + 1);

            if ((s->ca_8keys[j][i] % s->height) == 1 && (s->ca_8keys[j][i + 2] % s->height) == 1 && (s->ca_8keys[j][i + 4] % s->height) == 1) {
                if (max < rand) {
                    max = rand;
                    k  = i;
                }
            }
        }
        if (max > 0)
            note[j] = k;
    }
    k = 0;
    while (k < 8) {
        int j = 0;
        if (note[k] > 0) {
            s->last_note = note[k];
            for (int i = 0; i < 3; i++)
                notes[i] = s->note_map[(s->last_note + 2 * i) % s->height];
            multiple_notes (note_time, 0, 3, ON, notes, s);
            note_time += 4 * s->beat_dur / 8;
            while ( k + j < 8) {
                if (note[k+j] > 0 && note[k + j] == note[k]) {
                    note_time += 4 * s->beat_dur/8;
                    j++;
                }
                else
                    break;
            }
            multiple_notes (note_time, 0, 3, OFF, notes, s);
        }
        k += j + 1;
    }
}

static void ca_lead_upper_whole (void *t)
{
    FluidsynthmusicContext *s = t;
    int note_time = s->time_marker, note[8], k;

    for (int j = 0; j < 8; j++) {
        unsigned max = 0;

        note[j] = 0;
        for (int i = FFMAX(s->last_lead_note - 3, s->height/3); i < FFMIN(s->last_lead_note + 3, s->height); i++) {
            unsigned rand = (av_lfg_get(&s->r) * s->ca_8keys[j][i]) % (5 * i + 1);

            if (max < rand) {
                max = rand;
                k = i;
            }
        }
        if (max > 0)
            note[j] = k;
    }
    k = 0;
    while(k < 8){
        int j = 0;
        if (note[k] > 0) {
            s->last_lead_note = note[k];
            schedule_noteon (CA_LEAD, s->note_map[s->last_lead_note % s->height], note_time, s->velocity, s);
            note_time += 4 * s->beat_dur / 8;
            while ( k + j < 8) {
                if (note[k+j] > 0 && note[k + j] == note[k]) {
                    note_time += 4 * s->beat_dur / 8;
                    j++;
                }
                else
                    break;
            }
            schedule_noteoff(CA_LEAD, s->note_map[s->last_lead_note % s->height], note_time, s);
        }
        k += j+1;
    }
}

static void ca_lead_lower_whole (void *t)
{
    FluidsynthmusicContext *s = t;
    int note_time = s->time_marker, note[8], k;

    for (int j = 0; j < 8; j++) {
        unsigned max = 0;

        note[j] = 0;
        for (int i = FFMAX(s->last_lead_note - 3, s->height/3); i < FFMIN(s->last_lead_note + 3, s->height); i++) {
            unsigned rand = (av_lfg_get(&s->r) * s->ca_8keys[j][i]) % (5 * FFABS(s->height - i) + 1);

            if (max < rand) {
                max = rand;
                k = i;
            }
        }
        if (max > 0)
            note[j] = k;
    }
    k = 0;
    while(k < 8){
        int j = 0;
        if (note[k] > 0) {
            s->last_lead_note = note[k];
            schedule_noteon (CA_LEAD, s->note_map[s->last_lead_note % s->height], note_time, s->velocity, s);
            note_time += 4 * s->beat_dur / 8;
            while ( k + j < 8) {
                if (note[k+j] > 0 && note[k + j] == note[k]) {
                    note_time += 4 * s->beat_dur / 8;
                    j++;
                }
                else
                    break;
            }
            schedule_noteoff(CA_LEAD, s->note_map[s->last_lead_note % s->height], note_time, s);
        }
        k += j + 1;
    }
}

static void ca_lead_upper_eighth (void *t)
{
    FluidsynthmusicContext *s = t;
    int note_time = s->time_marker, note = s->last_lead_note;

    for (int j = 0; j < 8; j++) {
        unsigned max = 0;

        for (int i = FFMAX(s->last_lead_note - 3, s->height/3); i < FFMIN(s->last_lead_note + 3, s->height); i++) {
            unsigned rand = (av_lfg_get(&s->r) * s->ca_8keys[j][i]) % (5 * i + 1);

            if (max < rand) {
                max = rand;
                note = i;
            }
        }
        if (max > 0) {
            s->last_lead_note = note;
            schedule_noteon (CA_LEAD, s->note_map[s->last_lead_note % s->height], note_time, s->velocity, s);
            note_time += 4 * s->beat_dur / 8;
            schedule_noteoff(CA_LEAD, s->note_map[s->last_lead_note % s->height], note_time, s);
        }
    }
}

static void ca_lead_lower_eighth (void *t)
{
    FluidsynthmusicContext *s = t;
    int note_time = s->time_marker, note = s->last_lead_note;


    for (int j = 0; j < 8; j++) {
        unsigned max = 0;

        for (int i = FFMAX(s->last_lead_note - 3, s->height / 3); i < FFMIN(s->last_lead_note + 3, s->height); i++) {
            unsigned rand = (av_lfg_get(&s->r) * s->ca_8keys[j][i]) % (5 * FFABS(s->height - i) + 1);

            if (max < rand) {
                max = rand;
                note = i;
            }
        }
        if (max > 0) {
            s->last_lead_note = note;
            schedule_noteon (CA_LEAD, s->note_map[s->last_lead_note % s->height], note_time, s->velocity, s);
            note_time += 4 * s->beat_dur / 8;
            schedule_noteoff(CA_LEAD, s->note_map[s->last_lead_note%s->height], note_time, s);
        }
    }

}

static void schedule_ca_pattern(void *t)
{
    FluidsynthmusicContext *s = t;

    for (int i = 0; i < 8; i++)
        s->ca_generate(s->ca_cells, s->ca_nextgen, s->ca_8keys[i], s->ca_neighbours, s->ca_ruleset, s->ca_nsize, s->height, &s->r);
    s->ca_bass(s);
    s->ca_chords(s);
    s->ca_lead(s);
    play_percussion(s);
    s->time_marker += 4 * s->beat_dur;

}

/*---Rhythm---*/
/*Stochastically subdivide and set value to 1*/
static void divvy(int lo, int hi, FluidsynthmusicContext *s)
{
    int mid;
    unsigned rand = av_lfg_get(&s->r);

    mid = (lo + hi) >> 1;
    s->p_instr[lo] = 1;
    if (rand % 101 < s->p_density && hi - lo > 1) {
        divvy(lo, mid, s);
        divvy(mid, hi, s);
    }
}

static int get_p_instr(FluidsynthmusicContext *s)
{
    int instr = av_lfg_get(&s->r) % 4;

    switch (instr) {
        case 0: instr = drums[av_lfg_get(&s->r) % FF_ARRAY_ELEMS(drums)];break;
        case 1: instr = toms[av_lfg_get(&s->r) % FF_ARRAY_ELEMS(toms)];break;
        case 2: instr = cymbals[av_lfg_get(&s->r) % FF_ARRAY_ELEMS(cymbals)];break;
        case 3: instr = hi_hats[av_lfg_get(&s->r) % FF_ARRAY_ELEMS(hi_hats)];break;
    }
    return instr;
}

/*Reference taken from :
http://cgm.cs.mcgill.ca/~godfried/publications/Hawaii-Paper-Rhythm-Generation.pdf
Reflect the first half of the pattern in second half*/
static void padriddle(FluidsynthmusicContext *s)
{
    divvy(0, s->p_maxres, s);
    for (int i = 0; i < s->p_maxres / 2; i++) {
        s->p_instr[i] = get_p_instr(s) * s->p_instr[i];
        s->p_instr[s->p_maxres / 2 + i] = s->p_instr[i];
        s->p_beats[i] = s->p_maxres;
        s->p_beats[s->p_maxres / 2 + i] = s->p_maxres;
    }
}

/*This will give beats of the type [xxx.] or [xxxxxxx.]*/
static void alternate(FluidsynthmusicContext *s)
{
    int i, rep_size, newsize, rep_array[] = {4, 8, 16, 32};

    for (i = 0; i < 4; i++) {
        if (s->p_maxres < rep_array[i])
            break;
    }

    rep_size = rep_array[av_lfg_get(&s->r) % i];
    newsize = s->p_maxres - s->p_maxres / rep_size;
    divvy(0, newsize, s);
    for (int i = 0; i < newsize; i++) {
        s->p_instr[i] = s->p_instr[i] * get_p_instr(s);
        if ((i + 1) % (rep_size - 1) == 0)
            s->p_beats[i] = s->p_maxres / 2;
        else
            s->p_beats[i] = s->p_maxres;
    }

    s->p_maxres = newsize;
}

/*Toggle between the instruments in the first half and second half*/
static void toggle(FluidsynthmusicContext *s)
{
    int instr1 = get_p_instr(s), instr2 = get_p_instr(s);
    divvy(0, s->p_maxres/2, s);
    for (int i = 0; i < s->p_maxres / 2; i++) {
        s->p_instr[i] = instr1 * s->p_instr[i];
        s->p_instr[s->p_maxres / 2 + i] = instr2 * s->p_instr[i] / instr1;
        s->p_beats[i] = s->p_maxres;
        s->p_beats[s->p_maxres / 2 + i] = s->p_maxres;
    }
}

static void alternate_n_padriddle(FluidsynthmusicContext *s)
{
    alternate(s);
    for (int i = 0; i < s->p_maxres / 2; i++) {
        s->p_instr[s->p_maxres / 2 + i] = s->p_instr[i];
    }
}

static void toggle_n_padriddle(FluidsynthmusicContext *s)
{
    s->p_maxres = s->p_maxres / 2;
    toggle(s);
    s->p_maxres = s->p_maxres * 2;
    for (int i = 0; i < s->p_maxres / 2; i++) {
        s->p_instr[s->p_maxres / 2 + i] = s->p_instr[i];
        s->p_beats[s->p_maxres / 2 + i] = s->p_beats[i];
    }
}

static void alternate_n_toggle(FluidsynthmusicContext *s)
{
    int instr1 = get_p_instr(s), instr2 = get_p_instr(s);

    alternate(s);
    for (int i = 0; i < s->p_maxres / 2; i++) {
        s->p_instr[i] = instr1 * FFMIN(1, s->p_instr[i]);
        s->p_instr[s->p_maxres / 2 + i] = instr2 * FFMIN(1, s->p_instr[s->p_maxres / 2 + i]);
    }
}

static void schedule_r_pattern(void *t)
{
    FluidsynthmusicContext *s = t;
    int note_time = s->time_marker;

    for (int i = 0; i < s->p_maxres; i++) {
        schedule_noteon(PERCUSSION, s->p_instr[i], note_time, s->percussion_velocity, s);
        note_time += 4 * s->beat_dur / s->p_beats[i];
        schedule_noteoff(PERCUSSION, s->p_instr[i], note_time, s);
    }

    s->time_marker += 4 * s->beat_dur;
}

static void sequencer_callback(unsigned int time, fluid_event_t *event, fluid_sequencer_t *seq, void *data)
{
    FluidsynthmusicContext *s = data;

    schedule_timer_event(data);
    s->schedule_pattern(data);
}

static int get_scale (FluidsynthmusicContext *s)
{
    int s_size, x[7];

    switch (s->scale_name[0]) {
        case 'C': x[0] = C3; break;
        case 'D': x[0] = D3; break;
        case 'E': x[0] = E3; break;
        case 'F': x[0] = F3; break;
        case 'G': x[0] = G3; break;
        case 'A': x[0] = A3; break;
        case 'B': x[0] = B3; break;
        default: x[0] = C3; break;
    }

    switch (s->scale_name[1]) {
        case 'b': x[0] -= 1; break;
        case 's': x[0] += 1; break;
    }

    if (strcmp(s->scale_name+2, "major") == 0 || strcmp(s->scale_name+3, "major") == 0) {
        for (int i = 0; i < FF_ARRAY_ELEMS(major_increment); i++)
            x[i + 1] = x[i] + major_increment[i];
        s_size = 7;
    }
    else if (strcmp(s->scale_name+2, "n_minor") == 0 || strcmp(s->scale_name+3, "n_minor") == 0) {
        for (int i = 0; i < FF_ARRAY_ELEMS(natural_minor_increment); i++)
            x[i + 1] = x[i] + natural_minor_increment[i];
        s_size = 7;
    }
    else if (strcmp(s->scale_name+2, "m_minor") == 0 || strcmp(s->scale_name+3, "m_minor") == 0) {
        for (int i = 0; i < FF_ARRAY_ELEMS(melodic_minor_increment); i++)
            x[i + 1] = x[i] + melodic_minor_increment[i];
        s_size = 7;
    }
    else if (strcmp(s->scale_name+2, "h_minor") == 0 || strcmp(s->scale_name+3, "h_minor") == 0) {
        for (int i = 0; i < FF_ARRAY_ELEMS(harmonic_minor_increment); i++)
            x[i + 1] = x[i] + harmonic_minor_increment[i];
        s_size = 7;
    }
    else if (strcmp(s->scale_name+2, "p_major") == 0 || strcmp(s->scale_name+3, "p_major") == 0) {
        for (int i = 0; i < FF_ARRAY_ELEMS(major_pentatonic_increment); i++)
            x[i + 1] = x[i] + major_pentatonic_increment[i];
        s_size = 5;
    }
    else if (strcmp(s->scale_name+2, "p_minor") == 0 || strcmp(s->scale_name+3, "p_minor") == 0) {
        for (int i = 0; i < FF_ARRAY_ELEMS(minor_pentatonic_increment); i++)
            x[i + 1] = x[i] + minor_pentatonic_increment[i];
        s_size = 5;
    }
    else if (strcmp(s->scale_name+2, "blues") == 0 || strcmp(s->scale_name+3, "blues") == 0) {
        for (int i = 0; i < FF_ARRAY_ELEMS(blues_increment); i++)
            x[i + 1] = x[i] + blues_increment[i];
        s_size = 6;
    }
    else {
        av_log(s, AV_LOG_WARNING, "scale %s not found! defaulting to a major scale\n", s->scale_name);
        for (int i = 0; i < FF_ARRAY_ELEMS(major_increment); i++)
            s->scale[i + 1] = s->scale[i] + major_increment[i];
        s_size = 7;
    }

    if (!(s->scale = av_malloc(s_size * sizeof(int))))
        return AVERROR(ENOMEM);
    memcpy(s->scale, x, s_size * sizeof(int));

    return s_size;
}

static int find_instrument(char *instrument, FluidsynthmusicContext *s)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(GM_instrument_list); i++)
        if (strcmp(GM_instrument_list[i], instrument) == 0)
            return i;

    av_log(s, AV_LOG_WARNING, "instrument %s "
           "not found! defaulting to Acoustic-Grand\n", instrument);
    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    FluidsynthmusicContext *s = ctx->priv;
    int sfont_id, copy, i = 1, j = 1, t = 0, s_size;

    /*Initialise the fluidsynth settings object followed by synthesizer*/
    s->settings = new_fluid_settings();
    if (s->settings == NULL) {
        av_log(s, AV_LOG_ERROR, "Failed to create the fluidsynth settings");
        return AVERROR_EXTERNAL;
    }

    s->synth = new_fluid_synth(s->settings);
    if (s->synth == NULL) {
        av_log(s, AV_LOG_ERROR, "Failed to create the fluidsynth synth");
        return AVERROR_EXTERNAL;
    }

    sfont_id = fluid_synth_sfload(s->synth, s->sfont, 1);
    if (sfont_id == FLUID_FAILED) {
        av_log(s, AV_LOG_ERROR, "Loading the Soundfont Failed");
        return AVERROR_EXTERNAL;
    }

    if (!(s->riffs = av_malloc(sizeof(riff))))
        return AVERROR(ENOMEM);
    if (!(s->prevgen = av_calloc(L_MAX_LENGTH * 2, sizeof(*s->nextgen))))
        return AVERROR(ENOMEM);
    if (!(s->nextgen = av_calloc(L_MAX_LENGTH * 2, sizeof(*s->nextgen))))
        return AVERROR(ENOMEM);
    if (!(s->system = av_calloc(L_MAX_LENGTH, sizeof(*s->system))))
        return AVERROR(ENOMEM);
    strcpy(s->prevgen, s->axiom);

    s->framecount = 0;
    s->sequencer = new_fluid_sequencer2(0);
    /* register the synth with the sequencer */
    s->synth_destination = fluid_sequencer_register_fluidsynth(s->sequencer, s->synth);
    /* register the client name and callback */
    s->client_destination = fluid_sequencer_register_client(s->sequencer, "fluidsynthmusic", sequencer_callback, s);
    s->time_marker = fluid_sequencer_get_tick(s->sequencer);
    /*get the beat duration in TICKS     1 quarter note per beat*/
    s->beat_dur = 60000/s->beats_pm;
    /*get change interval in frames/sec*/
    s->changerate = (4 * s->beat_dur) * s->sample_rate / s->nb_samples;
    if (s->changerate < 1.0)
        s->changerate = 1.0;

    s->lstate = 0;
    s->max = 0;
    s->last_note = 0;
    s->last_bass_note = 0;
    s->last_lead_note = s->height / 2;
    s->numriffs = sizeof(riff)/(NPR * sizeof(int));
    if (s->seed == -1)
        s->seed = av_get_random_seed();
    av_lfg_init(&s->r, s->seed);

    for (int i = 0; i < s->numriffs * NPR ; i++)
        s->riffs[i] = riff[i];

    s->ca_nsize = 0;
    copy = s->ca_ruletype;
    while (copy > 0) {
        if (copy % 2 == 1)
            s->ca_nsize++;
        copy = copy >> 1;
    }
    if (!(s->ca_neighbours = av_malloc(sizeof(int) * s->ca_nsize)))
        return AVERROR(ENOMEM);
    if (!(s->ca_ruleset = av_malloc(sizeof(int) * (1 << s->ca_nsize))))
        return AVERROR(ENOMEM);
    for (int k = 0; k < 8; k++)
        if (!(s->ca_8keys[k] = av_malloc(s->height * sizeof(int))))
            return AVERROR(ENOMEM);
    if (!(s->note_map = av_malloc(sizeof(int) * s->height)))
        return AVERROR(ENOMEM);
    s_size = get_scale(s);
    if (s_size == AVERROR(ENOMEM))
        return AVERROR(ENOMEM);
    copy = s->ca_ruletype;

    /*The neighbouring cells on which cells of next generation is determined as
      in http://tones.wolfram.com/about/how-it-works*/
    while (copy > 0) {
        if (copy % 2 == 1){
            if (i % 2 == 0)
                s->ca_neighbours[(s->ca_nsize - 1) / 2 + j / 2] = -1 * (i/2);
            else
                s->ca_neighbours[(s->ca_nsize - 1) / 2 - j / 2] = (i/2);
            j++;
        }
        copy = copy >> 1;
        i++;
    }
    copy = s->ca_rule;
    i = 0;
    while (i != (1 << s->ca_nsize)) {
        s->ca_ruleset[i++] = copy % 2;
        copy = copy >> 1;
    }

    /*In cellular automaton, the middle portion(s->height) is mapped to a scale
      The lower and upper octaves are mapped by subtracting and adding 12 respectively*/
    j = s_size/2 - (s->height+1)/4;
    for (i = 0; i < s->height; i++) {
        if (j < 0)
            s->note_map[i] = s->scale[s_size + j % s_size] - 12 * (int)((j * -1.0) / s_size + 1) ;
        else
            s->note_map[i] = s->scale[j % s_size] + 12 * (int)((j * 1.0) / s_size);
        j++;
    }

    for (i = 0; i < 32; i++)
        s->ca_cells[i] = av_lfg_get(&s->r) % 2;

    if (!(s->track = av_malloc(sizeof(int) * MAX_TRACK_SIZE)))
        return AVERROR(ENOMEM);

    for (t = 0 ; t < FF_ARRAY_ELEMS(percussion_tracks) ; t++)
        if (strcmp(percussion_tracks[t], s->track_name) == 0)
            break;

    switch (t) {
        case 0: memcpy(s->track, Track1, sizeof(Track1)); break;
        case 1: memcpy(s->track, Track2, sizeof(Track2)); break;
        case 2: memcpy(s->track, Track3, sizeof(Track3)); break;
        case 3: memcpy(s->track, Track4, sizeof(Track4)); break;
        case 4: memcpy(s->track, Track5, sizeof(Track5)); break;
        case 5: memcpy(s->track, Track6, sizeof(Track6)); break;
        case 6: memcpy(s->track, Track7, sizeof(Track7)); break;
        case 7: memcpy(s->track, Track8, sizeof(Track8)); break;
        case 8: memcpy(s->track, Track9, sizeof(Track9)); break;
        case 9: memcpy(s->track, Track10, sizeof(Track10)); break;
        case 10: memcpy(s->track, Track11, sizeof(Track11)); break;
        case 11: memcpy(s->track, Track12, sizeof(Track12)); break;
        default: av_log(s, AV_LOG_WARNING, "percussion track %s "
                        "not found! defaulting to Metronome\n", s->track_name);
                 memcpy(s->track, Track12, sizeof(Track12)); break;
    }

    switch (s->ca_boundary) {
        case INFINITE: s->ca_generate = infinite_generate; break;
        case CYCLIC: s->ca_generate = cyclic_generate; break;
    }

    switch (s->ca_bass_name) {
        case B_LOWEST_NOTES: s->ca_bass = ca_bass_lowest_notes; break;
        case B_LOWER_EIGHTH: s->ca_bass = ca_bass_lower_eighth; break;
    }

    switch (s->ca_chords_name) {
        case C_EIGHTH: s->ca_chords = ca_chords_eighth; break;
        case C_WHOLE: s->ca_chords = ca_chords_whole; break;
    }

    switch (s->ca_lead_name) {
        case L_UPPER_EIGHTH: s->ca_lead = ca_lead_upper_eighth; break;
        case L_UPPER_WHOLE: s->ca_lead = ca_lead_upper_whole; break;
        case L_LOWER_EIGHTH: s->ca_lead = ca_lead_lower_eighth; break;
        case L_LOWER_WHOLE: s->ca_lead = ca_lead_lower_whole; break;
    }

    if (!(s->p_instr = av_malloc(s->p_maxres * sizeof(int))))
        return AVERROR(ENOMEM);
    if (!(s->p_beats = av_malloc(s->p_maxres * sizeof(int))))
        return AVERROR(ENOMEM);
    memset(s->p_instr, 0, s->p_maxres * sizeof(int));
    memset(s->p_beats, 0, s->p_maxres * sizeof(int));

    switch (s->p_algorithm) {
        case PADRIDDLE: padriddle(s); break;
        case ALTERNATE: alternate(s); break;
        case TOGGLE: toggle(s); break;
        case ALTPAD: alternate_n_padriddle(s); break;
        case TOGPAD: toggle_n_padriddle(s); break;
        case TOGALT: alternate_n_toggle(s); break;
    }

    switch (s->algorithm) {
        case RIFFS: s->schedule_pattern = schedule_riff_pattern; break;
        case LSYSTEM: schedule_0L_pattern(s); s->schedule_pattern = schedule_L_pattern; break;
        case CA: s->schedule_pattern = schedule_ca_pattern; break;
        case RHYTHM: s->schedule_pattern = schedule_r_pattern; break;
    }

    instrument_select(find_instrument(s->instrument, s), s->time_marker, RIFFNL, s);
    instrument_select(find_instrument(s->bass_instr, s), s->time_marker, CA_BASS, s);
    instrument_select(find_instrument(s->chords_instr, s), s->time_marker, CA_CHORDS, s);
    instrument_select(find_instrument(s->lead_instr, s), s->time_marker, CA_LEAD, s);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    FluidsynthmusicContext *s = ctx->priv;

    delete_fluid_sequencer(s->sequencer);
    delete_fluid_synth(s->synth);
    delete_fluid_settings(s->settings);
    av_freep(&s->riffs);
    av_freep(&s->prevgen);
    av_freep(&s->nextgen);
    av_freep(&s->system);
    av_freep(&s->ca_ruleset);
    av_freep(&s->ca_neighbours);
    av_freep(&s->note_map);
    av_freep(&s->scale);
    av_freep(&s->p_instr);
    av_freep(&s->p_beats);
    av_freep(&s->track);
    for (int k = 0; k < 8; k++)
        av_freep(&s->ca_8keys[k]);
}

static av_cold int config_props(AVFilterLink *outlink)
{
    FluidsynthmusicContext *s = outlink->src->priv;

    if (s->duration == 0)
        s->infinite = 1;

    s->duration = av_rescale(s->duration, s->sample_rate, AV_TIME_BASE);

    if (s->framecount == INT_MAX)
        s->framecount = 0;

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *outlink = ctx->outputs[0];
    FluidsynthmusicContext *s = ctx->priv;
    AVFrame *frame;
    int  nb_samples;

    if (!s->infinite && s->duration <= 0) {
        return AVERROR_EOF;
    } else if (!s->infinite && s->duration < s->nb_samples) {
        nb_samples = s->duration;
    } else {
        nb_samples = s->nb_samples;
    }

    if (!(frame = ff_get_audio_buffer(outlink, nb_samples)))
        return AVERROR(ENOMEM);

    if (s->framecount % ((int)s->changerate) == 0) {
        s->schedule_pattern(s);
        schedule_timer_event(s);
    }

    fluid_synth_write_float(s->synth, nb_samples, frame->data[0], 0, 2, frame->data[0], 1, 2);

    if (!s->infinite)
        s->duration -= nb_samples;

    s->framecount++;
    frame->pts = s->pts;
    s->pts += nb_samples;
    return ff_filter_frame(outlink, frame);
}

static av_cold int query_formats(AVFilterContext *ctx)
{
    FluidsynthmusicContext *s = ctx->priv;
    static const int64_t chlayouts[] = { AV_CH_LAYOUT_STEREO, -1 };
    int sample_rates[] = { s->sample_rate, -1 };
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_NONE};
    AVFilterFormats *formats;
    AVFilterChannelLayouts *layouts;
    int ret;

    formats = ff_make_format_list(sample_fmts);
    if (!formats)
        return AVERROR(ENOMEM);

    ret = ff_set_common_formats (ctx, formats);
    if (ret < 0)
        return ret;

    layouts = avfilter_make_format64_list(chlayouts);
    if (!layouts)
        return AVERROR(ENOMEM);

    ret = ff_set_common_channel_layouts(ctx, layouts);
    if (ret < 0)
        return ret;

    formats = ff_make_format_list(sample_rates);
    if (!formats)
        return AVERROR(ENOMEM);
    return ff_set_common_samplerates(ctx, formats);
}

static const AVFilterPad fluidsynthmusic_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .config_props  = config_props,
    },
    { NULL }
};

AVFilter ff_asrc_fluidsynthmusic = {
    .name          = "fluidsynthmusic",
    .description   = NULL_IF_CONFIG_SMALL("Generate algorithmic music."),
    .query_formats = query_formats,
    .init          = init,
    .uninit        = uninit,
    .activate      = activate,
    .priv_size     = sizeof(FluidsynthmusicContext),
    .inputs        = NULL,
    .outputs       = fluidsynthmusic_outputs,
    .priv_class    = &fluidsynthmusic_class,
};
