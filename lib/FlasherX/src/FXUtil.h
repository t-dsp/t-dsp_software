//******************************************************************************
// FXUTIL.H -- FlasherX utility functions
//******************************************************************************
#ifndef FXUTIL_H_
#define FXUTIL_H_

void read_ascii_line( Stream *serial, char *line, int maxbytes );
void update_firmware( Stream *in, Stream *out,
			uint32_t buffer_addr, uint32_t buffer_size );
// LOCAL MOD (T-DSP): true if the last read_ascii_line() aborted on an inter-byte
// timeout (stalled relay). update_firmware() checks this to bail cleanly.
bool fx_read_timed_out();

#endif
