// OpmBank.h — parser for VOPM ".opm" patch bank files into OpmVoice[].
//
// The .opm format (VOPM / vgmrips) is plain text: a list of voices, each a header
// line plus an LFO, a channel, and four operator lines. One voice looks like:
//
//   @:0 Instrument Name
//   LFO: LFRQ AMD PMD WF NFRQ
//   CH:  PAN FL CON AMS PMS SLOT NE
//   M1:  AR D1R D2R RR D1L TL KS MUL DT1 DT2 AME
//   C1:  AR D1R D2R RR D1L TL KS MUL DT1 DT2 AME
//   M2:  AR D1R D2R RR D1L TL KS MUL DT1 DT2 AME
//   C2:  AR D1R D2R RR D1L TL KS MUL DT1 DT2 AME
//
// We map CON->algorithm, FL->feedback, and each operator by LABEL to a hardware
// slot (M1->0, M2->1, C1->2, C2->3, i.e. register offset ch + slot*8) so file
// line-ordering can't scramble the patch. LFO / PAN / SLOT / NE / AMS / PMS are
// parsed-past for now (this engine doesn't yet apply patch-level LFO settings).
//
// The parser is tolerant of CRLF line endings, blank lines, extra whitespace, and
// unrecognized lines. It does no I/O — hand it a text buffer (read a whole .opm
// file off the SD card into RAM, then parse). Banks hold up to 128 voices; parse
// as many as fit in the caller's array.

#pragma once

#include <stddef.h>
#include "OpmVoice.h"

namespace tdsp {
namespace ymfmopm {

// Parse a .opm text bank into out[] (up to maxVoices). Returns the number of
// voices parsed (0 if the text contains none).
int parseOpmBank(const char *text, size_t len, OpmVoice *out, int maxVoices);

} // namespace ymfmopm
} // namespace tdsp
