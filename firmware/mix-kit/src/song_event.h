// song_event.h — one packed note event of a transcoded MIDI song.
// Shared by every generated <song>_mid.h so the struct is defined exactly once.
#pragma once
#include <stdint.h>

// deltaMs since the previous event, then the note + velocity. vel 0 = note-off;
// note 0 with vel 0 = rest padding (for deltas that exceed 60000ms).
struct SongEv { uint16_t dms; uint8_t note; uint8_t vel; };
