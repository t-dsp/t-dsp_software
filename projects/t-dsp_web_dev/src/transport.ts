// SLIP framing + serial-stream demultiplexer.
//
// The firmware multiplexes two things on one USB CDC stream:
//   - Binary OSC frames, SLIP-encoded (RFC 1055). Frames open and close with 0xC0.
//   - Plain ASCII text (boot banners, Serial.println debug, CLI replies). Newline-terminated.
//
// We pull the discriminator from planning/osc-mixer-foundation/02-osc-protocol.md:
//   "If the byte is 0xC0 (SLIP END), it begins a SLIP frame. Otherwise, accumulate
//    into a line buffer until \n."
//
// CNMAT/OSC's SLIPEncodedSerial wraps every frame with leading + trailing 0xC0,
// so the leading END is always present and the demux is unambiguous.

const SLIP_END = 0xc0;
const SLIP_ESC = 0xdb;
const SLIP_ESC_END = 0xdc;
const SLIP_ESC_ESC = 0xdd;

export function slipEncode(payload: Uint8Array): Uint8Array {
  // Worst case: every byte gets escaped + 2 framing bytes.
  const out: number[] = [SLIP_END];
  for (const b of payload) {
    if (b === SLIP_END) {
      out.push(SLIP_ESC, SLIP_ESC_END);
    } else if (b === SLIP_ESC) {
      out.push(SLIP_ESC, SLIP_ESC_ESC);
    } else {
      out.push(b);
    }
  }
  out.push(SLIP_END);
  return new Uint8Array(out);
}

type DemuxState = 'IDLE' | 'FRAME' | 'ESC';

export class StreamDemuxer {
  private state: DemuxState = 'IDLE';
  private frameBuf: number[] = [];
  private textBuf: number[] = [];
  private decoder = new TextDecoder('utf-8', { fatal: false });

  constructor(
    private onFrame: (frame: Uint8Array) => void,
    private onText: (line: string) => void,
  ) {}

  feed(bytes: Uint8Array): void {
    for (let i = 0; i < bytes.length; i++) {
      this.feedByte(bytes[i]);
    }
  }

  private feedByte(b: number): void {
    if (this.state === 'IDLE') {
      if (b === SLIP_END) {
        this.flushText();
        this.state = 'FRAME';
        this.frameBuf.length = 0;
      } else if (b === 0x0a /* \n */) {
        this.flushText();
      } else if (b === 0x0d /* \r */) {
        // ignore
      } else {
        this.textBuf.push(b);
      }
      return;
    }

    if (this.state === 'FRAME') {
      if (b === SLIP_END) {
        if (this.frameBuf.length > 0) {
          this.onFrame(new Uint8Array(this.frameBuf));
        }
        // Empty frame (back-to-back END) is just a sync byte; drop and return to IDLE.
        this.frameBuf.length = 0;
        this.state = 'IDLE';
      } else if (b === SLIP_ESC) {
        this.state = 'ESC';
      } else {
        this.frameBuf.push(b);
      }
      return;
    }

    // ESC state
    if (b === SLIP_ESC_END) {
      this.frameBuf.push(SLIP_END);
    } else if (b === SLIP_ESC_ESC) {
      this.frameBuf.push(SLIP_ESC);
    } else {
      // Protocol violation. Recover by inserting the byte literal and resuming the frame.
      this.frameBuf.push(b);
    }
    this.state = 'FRAME';
  }

  private flushText(): void {
    if (this.textBuf.length === 0) return;
    const line = this.decoder.decode(new Uint8Array(this.textBuf));
    this.textBuf.length = 0;
    this.onText(line);
  }
}
