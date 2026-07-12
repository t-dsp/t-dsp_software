// opm_bank_data.h — a small, hand-authored VOPM ".opm" bank baked into flash.
//
// This is DEMO/TEST data written for this project (not ripped from anywhere), so
// it's license-clean to ship. It exercises the .opm parser end-to-end without an
// SD card and gives eight starter instruments. Real/large banks are dropped onto
// the SD card at /ymfm/*.opm and parsed with the same code path.
//
// Format is standard .opm (see lib/TDspYmfm/src/OpmBank.h): operator lines are
// AR D1R D2R RR D1L TL KS MUL DT1 DT2 AME; CH: is PAN FL CON AMS PMS SLOT NE.

#pragma once

static const char kBakedOpmBank[] = R"OPM(
@:0 Grand Piano
LFO: 0 0 0 0 0
CH: 64 5 5 0 0 120 0
M1: 31 12 4 7 6 34 1 1 3 0 0
C1: 31 9 3 7 3 8 1 1 0 0 0
M2: 31 14 5 7 8 40 1 3 3 0 0
C2: 31 9 3 7 3 6 1 1 0 0 0

@:1 Bright Brass
LFO: 0 0 0 0 0
CH: 64 6 4 0 0 120 0
M1: 24 8 0 9 4 30 0 1 3 0 0
C1: 26 6 0 9 2 6 0 1 0 0 0
M2: 24 8 0 9 4 32 0 1 3 0 0
C2: 26 6 0 9 2 6 0 1 0 0 0

@:2 E.Bass
LFO: 0 0 0 0 0
CH: 64 7 2 0 0 120 0
M1: 31 16 6 10 10 26 0 1 0 0 0
C1: 31 12 5 10 6 8 0 1 0 0 0
M2: 31 18 7 10 12 30 0 2 0 0 0
C2: 31 12 5 10 6 6 0 1 0 0 0

@:3 Strings
LFO: 0 0 0 0 0
CH: 64 0 7 0 0 120 0
M1: 18 4 2 6 2 20 0 1 3 0 0
C1: 18 4 2 6 2 22 0 1 0 0 0
M2: 18 4 2 6 2 24 0 2 6 0 0
C2: 18 4 2 6 2 20 0 1 0 0 0

@:4 Tubular Bell
LFO: 0 0 0 0 0
CH: 64 2 5 0 0 120 0
M1: 31 8 3 4 3 30 0 14 1 0 0
C1: 31 7 2 4 2 10 0 1 0 0 0
M2: 31 9 3 4 4 34 0 7 2 0 0
C2: 31 7 2 4 2 12 0 1 0 0 0

@:5 Clav
LFO: 0 0 0 0 0
CH: 64 7 4 0 0 120 0
M1: 31 20 8 8 12 28 0 3 3 0 0
C1: 31 16 6 8 8 10 0 1 0 0 0
M2: 31 22 9 8 12 32 0 4 3 0 0
C2: 31 16 6 8 8 8 0 1 0 0 0

@:6 Vibraphone
LFO: 0 0 0 0 0
CH: 64 0 7 0 0 120 0
M1: 31 10 2 5 3 16 0 1 0 0 0
C1: 31 9 2 5 2 14 0 2 0 0 0
M2: 31 11 3 5 4 20 0 4 0 0 0
C2: 31 9 2 5 2 16 0 1 0 0 0

@:7 Synth Lead
LFO: 0 0 0 0 0
CH: 64 6 0 0 0 120 0
M1: 31 6 0 8 2 32 0 1 3 0 0
C1: 31 6 0 8 2 20 0 1 0 0 0
M2: 31 6 0 8 2 24 0 2 6 0 0
C2: 28 5 0 8 2 4 0 1 0 0 0
)OPM";
