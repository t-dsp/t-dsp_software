// NoteParser.h — convert note-name filenames ("C4", "F#3", "Bb-1") to MIDI numbers.
//
// Used by the multisample slot to scan an SD directory like:
//   /samples/piano/C4.wav
//   /samples/piano/F#5.wav
//   /samples/piano/A0.wav
// and build a MIDI-note → file map without a separate manifest.
//
// MIDI convention: C-1 = 0, A0 = 21, C4 = 60 (middle C), C8 = 108.
// Sharps and flats both supported on input ("F#4" == "Gb4" == 66).

#pragma once

#include <stdint.h>
#include <string.h>
#include <ctype.h>

namespace tdsp_synth {

// Returns -1 if `name` doesn't parse as a note. `name` should be a
// null-terminated string of the form "<L>[#|b]<octave>" where L is
// A..G (case-insensitive) and octave is a (possibly negative) integer.
inline int parseNoteName(const char *name) {
    if (!name || !*name) return -1;

    // Pitch class for each letter (semitones above C).
    static const int kSemis[7] = { 9, 11, 0, 2, 4, 5, 7 };  // A,B,C,D,E,F,G

    char c = (char)toupper((unsigned char)*name++);
    if (c < 'A' || c > 'G') return -1;
    int semis = kSemis[c - 'A'];

    // Optional sharp / flat.
    if (*name == '#') { semis += 1; ++name; }
    else if (*name == 'b' || *name == 'B') { semis -= 1; ++name; }

    // Octave — optionally signed integer.
    if (!*name) return -1;
    int sign = 1;
    if (*name == '-') { sign = -1; ++name; }
    if (!isdigit((unsigned char)*name)) return -1;
    int octave = 0;
    while (isdigit((unsigned char)*name)) {
        octave = octave * 10 + (*name - '0');
        ++name;
    }
    octave *= sign;

    if (*name != '\0') return -1;  // trailing garbage rejects

    int midi = (octave + 1) * 12 + semis;
    if (midi < 0 || midi > 127) return -1;
    return midi;
}

}  // namespace tdsp_synth
