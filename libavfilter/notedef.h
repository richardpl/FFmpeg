/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#ifndef AVFILTER_NOTEDEF_H
#define AVFILTER_NOTEDEF_H
#include <stdlib.h>
#include<string.h>

/*Define notes as per the General Midi specifications
 *_ = minus, s = sharp, b = flat
 *   H = hold tone, R = Rest*/
enum notes { C_1, Cs_1, Db_1 = 1, D_1, Ds_1, Eb_1 =3, E_1, F_1, Fs_1, Gb_1 = 6, G_1, Gs_1, Ab_1 = 8, A_1, As_1, Bb_1 =10, B_1,
    C0, Cs0, Db0 = 13, D0, Ds0, Eb0 = 15, E0, F0, Fs0, Gb0 = 18, G0, Gs0, Ab0 = 20, A0, As0, Bb0 = 22, B0,
    C1, Cs1, Db1 = 25, D1, Ds1, Eb1 = 27, E1, F1, Fs1, Gb1 = 30, G1, Gs1, Ab1 = 32, A1, As1, Bb1 = 34, B1,
    C2, Cs2, Db2 = 37, D2, Ds2, Eb2 = 39, E2, F2, Fs2, Gb2 = 42, G2, Gs2, Ab2 = 44, A2, As2, Bb2 = 46, B2,
    C3, Cs3, Db3 = 49, D3, Ds3, Eb3 = 51, E3, F3, Fs3, Gb3 = 54, G3, Gs3, Ab3 = 56, A3, As3, Bb3 = 58, B3,
    C4, Cs4, Db4 = 61, D4, Ds4, Eb4 = 63, E4, F4, Fs4, Gb4 = 66, G4, Gs4, Ab4 = 68, A4, As4, Bb4 = 70, B4,
    C5, Cs5, Db5 = 73, D5, Ds5, Eb5 = 75, E5, F5, Fs5, Gb5 = 78, G5, Gs5, Ab5 = 80, A5, As5, Bb5 = 82, B5,
    C6, Cs6, Db6 = 85, D6, Ds6, Eb6 = 87, E6, F6, Fs6, Gb6 = 90, G6, Gs6, Ab6 = 92, A6, As6, Bb6 = 94, B6,
    C7, Cs7, Db7 = 97, D7, Ds7, Eb7 = 99, E7, F7, Fs7, Gb7 = 102, G7, Gs7, Ab7 = 104, A7, As7, Bb7 = 106, B7,
    C8, Cs8, Db8 = 109, D8, Ds8, Eb8 = 111, E8, F8, Fs8, Gb8 = 114, G8, Gs8, Ab8 = 116, A8, As8, Bb8 = 118, B8,
    C9, Cs9, Db9 = 121, D9, Ds9, Eb9 = 123, E9, F9, Fs9, Gb9 = 126, G9, H, R};

const char *GM_instrument_list[128] = {"Acoustic-Grand", "Bright-Acoustic", "Electric-Grand", "Honky-Tonk", "Electric-Piano-1", "Electric-Piano-2",
    "Harpsichord", "Clav", "Celesta", "Glockenspiel", "Music-Box", "Vibraphone", "Marimba", "Xylophone", "Tubular-Bells", "Dulcimer", "Drawbar-Organ",
    "Percussive-Organ", "Rock-Organ", "Church-Organ", "Reed-Organ", "Accordion", "Harmonica", "Tango-Accordion", "Acoustic-Guitar-nylon", "Acoustic-Guitar-steel",
    "Electric-Guitar-jazz","Electric-Guitar-clean", "Electric-Guitar-muted", "Overdriven-Guitar", "Distortion-Guitar", "Guitar-Harmonics",
    "Acoustic-Bass", "Electric-Bass-finger", "Electric-Bass-pick", "Fretless-Bass", "Slap-Bass-1", "Slap-Bass-2", "Synth-Bass-1", "Synth-Bass-2",
    "Violin", "Viola", "Cello", "Contrabass", "Tremolo-Strings", "Pizzicato-Strings", "Orchestral-Harp", "Timpani", "String-Ensemble-1", "String-Ensemble-2",
    "SynthStrings-1", "SynthStrings-2", "Choir-Aahs", "Voice-Oohs", "Synth-Voice", "Orchestra-Hit", "Trumpet", "Trombone", "Tuba",
    "Muted-Trumpet", "French-Horn", "Brass-Section", "SynthBrass-1", "SynthBrass-2", "Soprano-Sax", "Alto-Sax", "Tenor-Sax", "Baritone-Sax",
    "Oboe", "English-Horn", "Bassoon", "Clarinet", "Piccolo", "Flute", "Recorder", "Pan-Flute", "Blown-Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead-1-square", "Lead-2-sawtooth", "Lead-3-calliope", "Lead-4-chiff", "Lead-5-charang", "Lead-6-voice", "Lead-7-fifths", "Lead-8-bass+lead",
    "Pad-1-new-age", "Pad-2-warm", "Pad-3-polysynth", "Pad-4-choir", "Pad-5-bowed", "Pad-6-metallic", "Pad-7-halo", "Pad-8-sweep",
    "FX-1-rain", "FX-2-soundtrack", "FX-3-crystal", "FX-4-atmosphere", "FX-5-brightness", "FX-6-goblins", "FX-7-echoes", "FX-8-sci-fi",
    "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai", "Tinkle-Bell", "Agogo", "Steel-Drums", "Woodblock", "Taiko-Drum",
    "Melodic-Tom", "Synth-Drum", "Reverse-Cymbal", "Guitar-Fret-Noise", "Breath-Noise", "Seashore", "Bird-Tweet", "Telephone-Ring", "Helicopter",
    "Applause", "Gunshot"};

enum percussion_notes{Metronome_Click = 33, Metronome_Bell, Acoustic_Bass_Drum,  Bass_Drum_1,  Side_Stick,  Acoustic_Snare,  Hand_Clap,  Electric_Snare,  Low_Floor_Tom,
    Closed_Hi_Hat,  High_Floor_Tom,  Pedal_Hi_Hat,  Low_Tom,  Open_Hi_Hat,  Low_Mid_Tom,  Hi_Mid_Tom,  Crash_Cymbal_1,  High_Tom,  Ride_Cymbal_1,  Chinese_Cymbal,
    Ride_Bell, Tambourine,  Splash_Cymbal, Cowbell,  Crash_Cymbal_2, Vibraslap,  Ride_Cymbal_2, Hi_Bongo,  Low_Bongo,  Mute_Hi_Conga,  Open_Hi_Conga,  Low_Conga,
    High_Timbale,  Low_Timbale,  High_Agogo,  Low_Agogo, Cabasa, Maracas,  Short_Whistle,  Long_Whistle,  Short_Guiro,  Long_Guiro, Claves,  Hi_Wood_Block,
    Low_Wood_Block, Mute_Cuica,  Open_Cuica,  Mute_Triangle, Open_Triangle};

/*Assuming maximum of three percussion instruments can be played at the same time*/
/*Define some drum beats...
Beat: Whole note = 1, Half note = 2, Quater note = 4, ..., Number of beats: Whole note = 4/1, Half note = 4/2, Quater note = 4/4
Note 1 does not produce any sound*/
/*In tracks the 4 * i + 0 element is instrument1
  4 * i + 1 element is instrument2
  4 * i + 2 element is instrument3
  4 * i + 3 element is beat*/
const int Track1[] = {Bass_Drum_1, Ride_Cymbal_2, 1, 12,
    1, 1, 1, 12,
    Bass_Drum_1, 1, 1, 12,
    Electric_Snare, Ride_Cymbal_2, Pedal_Hi_Hat, 12,
    1, 1, 1, 12,
    Electric_Snare, Ride_Cymbal_2, 1, 12,
    Bass_Drum_1, Ride_Cymbal_2, 1, 12,
    1, 1, 1, 12,
    Bass_Drum_1, 1, 1, 12,
    Electric_Snare, Ride_Cymbal_2, Pedal_Hi_Hat, 12,
    1, 1, 1, 12,
    Electric_Snare, Ride_Cymbal_2, 1, 12};

const int Track2[] = {1, Ride_Cymbal_2, 1, 12,
    1, 1, 1, 12,
    Bass_Drum_1, 1, 1, 12,
    Electric_Snare, Ride_Cymbal_2, Pedal_Hi_Hat, 12,
    1, 1, 1, 12,
    Bass_Drum_1, Ride_Cymbal_2, 1, 12,
    1, Ride_Cymbal_2, 1, 12,
    1, 1, 1, 12,
    Bass_Drum_1, 1, 1, 12,
    Electric_Snare, Ride_Cymbal_2, Pedal_Hi_Hat, 12,
    1, 1, 1, 12,
    Bass_Drum_1, Ride_Cymbal_2, 1, 12};

const int Track3[] = {1, Ride_Cymbal_2, 1, 12,
    1, 1, 1, 12,
    1, 1, 1, 12,
    1, Ride_Cymbal_2, Pedal_Hi_Hat, 12,
    1, 1, 1, 12,
    1, Ride_Cymbal_2, 1, 12,
    1, Ride_Cymbal_2, 1, 12,
    Electric_Snare, 1, 1, 12,
    Electric_Snare, 1, 1, 12,
    1, Ride_Cymbal_2, Pedal_Hi_Hat, 12,
    1, 1, 1, 12,
    1, Ride_Cymbal_2, 1, 12};

const int Track4[] = {1, Ride_Cymbal_2, 1, 12,
    Electric_Snare, 1, 1, 12,
    Electric_Snare, 1, 1, 12,
    1, Ride_Cymbal_2, Pedal_Hi_Hat, 12,
    1, 1, 1, 12,
    1, Ride_Cymbal_2, 1, 12,
    1, Ride_Cymbal_2, 1, 12,
    Electric_Snare, 1, 1, 12,
    Electric_Snare, 1, 1, 12,
    1, Ride_Cymbal_2, Pedal_Hi_Hat, 12,
    1, 1, 1, 12,
    1, Ride_Cymbal_2, 1, 12};

const int Track5[] = {Electric_Snare, Ride_Cymbal_2, 1, 12,
    Electric_Snare, 1, 1, 12,
    Bass_Drum_1, 1, 1, 12,
    1, Ride_Cymbal_2, Pedal_Hi_Hat, 12,
    1, 1, 1, 12,
    1, Ride_Cymbal_2, 1, 12,
    Electric_Snare, Ride_Cymbal_2, 1, 12,
    Electric_Snare, 1, 1, 12,
    Bass_Drum_1, 1, 1, 12,
    1, Ride_Cymbal_2, Pedal_Hi_Hat, 12,
    1, 1, 1, 12,
    1, Ride_Cymbal_2, 1, 12};

const int Track6[] = {1, Ride_Cymbal_2, 1, 12,
    1, 1, 1, 12,
    Electric_Snare, 1, 1, 12,
    1, Ride_Cymbal_2, Pedal_Hi_Hat, 12,
    Electric_Snare, 1, 1, 12,
    Electric_Snare, Ride_Cymbal_2, 1, 12,
    1, Ride_Cymbal_2, 1, 12,
    1, 1, 1, 12,
    Bass_Drum_1, 1, 1, 12,
    Electric_Snare, Ride_Cymbal_2, Pedal_Hi_Hat, 12,
    1, 1, 1, 12,
    1, Ride_Cymbal_2, 1, 12};

const int Track7[] = {Bass_Drum_1, 1, Closed_Hi_Hat, 8,
    1, 1, Closed_Hi_Hat, 8,
    Acoustic_Snare, 1, Closed_Hi_Hat, 8,
    1, 1, Closed_Hi_Hat, 8,
    Bass_Drum_1, 1, Closed_Hi_Hat, 8,
    Bass_Drum_1, 1, Closed_Hi_Hat, 8,
    Acoustic_Snare, 1, Closed_Hi_Hat, 8,
    1, 1, Closed_Hi_Hat, 8};

const int Track8[] = {Bass_Drum_1, 1, Closed_Hi_Hat, 8,
    Bass_Drum_1, 1, Closed_Hi_Hat, 8,
    Acoustic_Snare, 1, Closed_Hi_Hat, 8,
    Bass_Drum_1, 1, Closed_Hi_Hat, 8,
    Bass_Drum_1, 1, Closed_Hi_Hat, 8,
    1, 1, Closed_Hi_Hat, 8,
    Bass_Drum_1, 1, Closed_Hi_Hat, 8,
    1, 1, Closed_Hi_Hat, 8};

const int Track9[] = {Bass_Drum_1, 1, Closed_Hi_Hat, 8,
    Bass_Drum_1, 1, Closed_Hi_Hat, 8,
    Acoustic_Snare, 1, Closed_Hi_Hat, 8,
    Bass_Drum_1, 1, Closed_Hi_Hat, 8,
    1, 1, Closed_Hi_Hat, 8,
    Bass_Drum_1, 1, Closed_Hi_Hat, 8,
    Acoustic_Snare, 1, Closed_Hi_Hat, 8,
    1, 1, Closed_Hi_Hat, 8};

const int Track10[] = {Bass_Drum_1, Acoustic_Snare, Closed_Hi_Hat, 8,
    1, 1, Closed_Hi_Hat, 8,
    Bass_Drum_1, Acoustic_Snare, Closed_Hi_Hat, 8,
    1, 1, Closed_Hi_Hat, 8,
    1, Acoustic_Snare, Closed_Hi_Hat, 8,
    Bass_Drum_1, 1, Closed_Hi_Hat, 8,
    1, Acoustic_Snare, Closed_Hi_Hat, 8,
    Bass_Drum_1, 1, Closed_Hi_Hat, 8};

const int Track11[] = {Bass_Drum_1, 1, Closed_Hi_Hat, 12,
    1, 1, 1, 12,
    1, 1, Closed_Hi_Hat, 12,
    Acoustic_Snare, 1, Closed_Hi_Hat, 12,
    1, 1, 1, 12,
    1, 1, Closed_Hi_Hat, 12,
    Bass_Drum_1, 1, Closed_Hi_Hat, 12,
    1, 1, 1, 12,
    1, 1, Closed_Hi_Hat, 12,
    Acoustic_Snare, 1, Closed_Hi_Hat, 12,
    1, 1, 1, 12,
    1, 1, Closed_Hi_Hat, 12};

const int Track12[] = {Metronome_Click, 1, 1, 4,
    Metronome_Click, 1, 1, 4,
    Metronome_Click, 1, 1, 4,
    Metronome_Click, 1, 1, 4};

const char *percussion_tracks[] = {"Jazz1", "Jazz2", "Jazz3", "Jazz4", "Jazz5", "Jazz6", "Rock1", "Rock2", "Rock3", "Rock4", "Shuffle", "Metronome"};

#define MAX_TRACK_SIZE 144
/*Define the riffs : 8 notes per riff
Reference: http://peterlangston.com/Papers/amc.pdf */
#define NPR 8
const int riff[] = {Eb4,  D4,  A4,  F4,  E4,  C5,  A4,  A4,  /*  0 */
    F4,  A4, Eb5,  D5,  E4,  A4,  C5,  A4,  /*  1 */
    Ab4,  A4,   H,  G5,   H,  Eb5,  C5,  E5,  /*  2 */
    Ab4,  A4,  B4,  C5, Eb5,  E5, Ab5,  A5,  /*  3 */
    A4,  Bb4,  B4,  C5, Db5,  D5, Eb5,  E5,  /*  4 */
    A4,  Bb4,  B4,  C5,  E5, Eb5,  D5,  C5,  /*  5 */
    A4,  B4,  C5,  A4,  B4,  C5,  D5,  B4,  /*  6 */
    A4,  B4,  C5,  D5, Eb5,  E5, Eb5,  C5,  /*  7 */
    A4,  C5,  D5, Eb5,  Gb5,  Ab5,  A5,  C6,  /*  8 Pat  Metheny  */
    A4,  C5, Eb5,  B4,  D5,  F5, Eb5,  C5,  /*  9 */
    A4,  C5,  E5,  G5,  B5,  A5,  G5,  E5,  /* 10  */
    A4,  C5,  E5,  A5,  G5, Eb5,  C5,  A4,  /* 11  */
    B4,  A4,  B4,  C5,  B4,  A4,  B4,  C5,  /* 12  */
    B4,  A4,  B4,  C5,  B4,  C5,  B4,  A4,  /* 13  */
    B4,  A4,  B4,  C5,  D5,  C5,  D5, Eb5,  /* 14  */
    C5, Ab4,  A4,  G5,  F5, Gb5, Eb5,  E5,  /* 15 Marty Cutler */
    C5,  D5,  C5,  B4,  C5,  B4,  A4,   H,  /* 16  */
    C5,  D5, Eb5,  C5,  D5, Eb5,  F5,  D5,  /* 17  */
    D5,  C5,  A4,  C5,  E5, Eb5,  D5,  C5,  /* 18  */
    D5,  C5,  D5, Eb5,  D5,  C5,  D5, Eb5,  /* 19  */
    D5,  Eb5,  E5,  F5, Gb5,  G5, Ab5,  A5,  /* 20  */
    D5, Eb5,  G5, Eb5,  D5,  C5,  B4,  C5,  /* 21 Charlie Keagle */
    D5,  Eb5,  A5,  D5,   H,  C5,  A4,  E4,  /* 22  */
    D5,  E5,  G5,  E5,  C5,   H,  D5,  A5,  /* 23  Lyle  Mays/Steve  Cantor  */
    Eb5,  D5, Eb5,  D5,   H,  C5,  A4,   H,  /* 24  */
    Eb5,  D5, Eb5,  F5, Eb5,  D5,  C5,  B4,  /* 25  */
    Eb5,  E5,  D5,  C5,  B4,  A4, Ab4,  A4,  /* 26  Richie  Shulberg  */
    Eb5,  E5,  A5,  C5,  B4,  E5,  A4,  A4,  /* 27  */
    Eb5,  Gb5,  E5,  A4,  B4,  D5,  C5,  E4,  /* 28  Django  Rheinhart  */
    E5,  A4,  C5, Ab4,  B4,  G4, Gb4,  E4,  /* 29  David  Levine  */
    E5,  Eb5,  D5,  C5,  B4,  C5,  D5,  F5,  /* 30  */
    G5,  E5,  D5,  B4, Eb5,   H,  C5,  A4,  /* 31  */
    G5,  E5,  D5, Gb5,  C5,   H,  A4,   H,  /* 32  Mike  Cross  */
    Ab5,  A5, Ab5,  A5, Ab5,  A5, Ab5,  A5,  /* 33  Django  Rheinhart  */
    A5,  E5,  C5,  G4,  C5,  E5,  A5,  A5,  /* 34  Django  Rheinhart  */
    A5,  E5,  C5,  A4,  G5, Eb5,  C5,  A4,  /* 35  */
    A5,  B5,  G5,  E5,  F5, Gb5,  G5, Ab5,  /* 36  */
    B5,  C6,  A5,  E5,  G5,  B5,  A5,   H,  /* 37  */
    B5,  D6,  C6,  E5, Ab5,  B5,  A5,  C5,  // 38  Django  Rheinhart
    C6,  B5,  A5,  G5, Gb5,  E5, Eb5,  C5} ;/* 39  */

typedef struct {
    int note;
    int dur;
} lsys;

#define L_MAX_LENGTH 65536

/*Define Scale Intervals*/
const int major_increment[] = {2, 2, 1, 2, 2, 2};
const int natural_minor_increment[] = {2, 1, 2, 2, 1, 2};
const int melodic_minor_increment[] = {2, 1, 2, 2, 2, 2};
const int harmonic_minor_increment[] = {2, 1, 2, 2, 1, 3};
const int major_pentatonic_increment[] = {2, 2, 3, 2};
const int minor_pentatonic_increment[] = {3, 2, 2, 3};
const int blues_increment[] = {3, 2, 1, 1, 3};

const int drums[] = {Bass_Drum_1, Acoustic_Bass_Drum, Acoustic_Snare, Electric_Snare};
const int toms[] = {Low_Floor_Tom, Low_Mid_Tom, Low_Tom, Hi_Mid_Tom, High_Floor_Tom, High_Tom};
const int cymbals[] = {Crash_Cymbal_1, Crash_Cymbal_2, Ride_Cymbal_1, Ride_Cymbal_2, Chinese_Cymbal, Splash_Cymbal};
const int hi_hats[] = {Pedal_Hi_Hat, Closed_Hi_Hat, Open_Hi_Hat};

#endif/*AVFILTER_NOTEDEF_H*/

