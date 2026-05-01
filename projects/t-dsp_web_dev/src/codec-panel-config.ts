// Declarative descriptor for the TAC5212 codec settings panel.
//
// The leaves and enum options here mirror the canonical /codec/tac5212/...
// address tree. When that subtree changes, update this file in lockstep —
// there is no other source of truth in the browser surface for codec leaves.
//
// Hierarchy (matches planning/tac5212-hw-dsp/PLAN.md §5):
//   Tab → Section → Control
// Each Section renders as a bordered card with a bold uppercase header,
// optional one-line subtitle, optional preset buttons, and optional
// collapse/expand. Controls are simple sliders/selects/toggles plus the
// composite `biquad` kind that renders 3 sliders + a live response curve.

export type BiquadType =
  | 'off'
  | 'peak'
  | 'low_shelf'
  | 'high_shelf'
  | 'low_pass'
  | 'high_pass'
  | 'band_pass'
  | 'notch';

export interface BiquadDesign {
  type: BiquadType;
  freqHz: number;
  gainDb: number;
  q: number;
}

export type Control =
  | { kind: 'enum'; label: string; help?: string; address: string; options: string[];
      disableSection?: { value: string; sectionName: string } }
  | { kind: 'toggle'; label: string; help?: string; address: string }
  | { kind: 'action'; label: string; help?: string; address: string }
  | { kind: 'range'; label: string; help?: string; address: string;
      min: number; max: number; step: number; unit: string }
  // Radio group that fans out each option to one or more OSC writes. Used
  // for "quick configuration" presets that touch multiple leaves at once
  // (e.g. set OUT1 and OUT2 to the same driver mode in one click). The
  // selected option is determined by listening to every address it writes:
  // an option is "current" only when all of its writes match the live
  // values. If the underlying leaves are out of sync (or were set to a
  // value not represented here), no option is selected.
  | { kind: 'radio'; label: string; help?: string;
      options: { label: string; writes: { address: string; value: string }[] }[] }
  | { kind: 'biquad'; label: string; help?: string;
      // Address base: per-band root, e.g. /codec/tac5212/dac/1/bq/2.
      // Widget sends `<base>/design s f f f` on slider change and
      // listens for echoes on both `<base>/design` and `<base>/coeffs`.
      addressBase: string;
      defaults: BiquadDesign }
  // Client-only checkbox bound to a named in-browser signal. No OSC
  // traffic; used for UI ganging where the firmware doesn't participate.
  // disableSection greys the named section while the toggle is on.
  | { kind: 'client_toggle'; label: string; help?: string;
      signalKey: 'eq_link';
      disableSection?: string };

// Preset buttons either (a) post a single OSC message to firmware, or
// (b) fan out to multiple biquad bands client-side (each entry sends
// `<addressBase>/design s f f f` with the same wire format the widget
// uses). Choose (b) when firmware has no preset handler.
export interface PresetButton {
  label: string;
  address?: string;
  arg?: string;
  designs?: { addressBase: string; design: BiquadDesign }[];
}

export interface Section {
  name: string;
  subtitle?: string;
  collapsible?: boolean;
  defaultCollapsed?: boolean;
  presets?: PresetButton[];
  controls: Control[];
  // When set, the entire section renders greyed-out and non-interactive,
  // and the reason becomes a browser tooltip on the card. Used for blocks
  // that exist in firmware but require external tooling (e.g. PPC3) to
  // configure safely.
  disabledReason?: string;
}

export interface Tab {
  name: string;
  sections: Section[];
}

// ---------------------------------------------------------------------------
// Helpers — section builders for repeated patterns.
// ---------------------------------------------------------------------------

// Three-band designs for the preset buttons. Order is [band1, band2, band3];
// the band shape is fixed (low_shelf / peak / high_shelf) per the EQ
// section's controls below.
const EQ_PRESETS: Record<'flat' | 'smooth' | 'bright' | 'dark', BiquadDesign[]> = {
  flat: [
    { type: 'low_shelf',  freqHz: 120,  gainDb:  0.0, q: 0.7 },
    { type: 'peak',       freqHz: 1000, gainDb:  0.0, q: 1.0 },
    { type: 'high_shelf', freqHz: 8000, gainDb:  0.0, q: 0.7 },
  ],
  smooth: [
    { type: 'low_shelf',  freqHz: 120,  gainDb:  1.5, q: 0.7 },
    { type: 'peak',       freqHz: 3000, gainDb: -2.5, q: 1.2 },
    { type: 'high_shelf', freqHz: 7000, gainDb: -3.0, q: 0.7 },
  ],
  bright: [
    { type: 'low_shelf',  freqHz: 100,  gainDb:  0.0, q: 0.7 },
    { type: 'peak',       freqHz: 3000, gainDb:  2.0, q: 1.2 },
    { type: 'high_shelf', freqHz: 8000, gainDb:  3.0, q: 0.7 },
  ],
  dark: [
    { type: 'low_shelf',  freqHz: 120,  gainDb:  2.0, q: 0.7 },
    { type: 'peak',       freqHz: 3000, gainDb: -1.5, q: 1.0 },
    { type: 'high_shelf', freqHz: 6000, gainDb: -4.0, q: 0.7 },
  ],
};

const eqPreset = (n: 1 | 2, label: string,
                  designs: BiquadDesign[]): PresetButton => ({
  label,
  designs: [
    { addressBase: `/codec/tac5212/dac/${n}/bq/1`, design: designs[0] },
    { addressBase: `/codec/tac5212/dac/${n}/bq/2`, design: designs[1] },
    { addressBase: `/codec/tac5212/dac/${n}/bq/3`, design: designs[2] },
  ],
});

const dacEqSection = (n: 1 | 2): Section => ({
  name: `Output ${n} — Tone shaping`,
  subtitle: 'Ear-fatigue defaults: gentle warmth, presence dip, soft top.',
  collapsible: true,
  presets: [
    eqPreset(n, 'Flat',   EQ_PRESETS.flat),
    eqPreset(n, 'Smooth', EQ_PRESETS.smooth),
    eqPreset(n, 'Bright', EQ_PRESETS.bright),
    eqPreset(n, 'Dark',   EQ_PRESETS.dark),
  ],
  controls: [
    { kind: 'biquad', label: 'Band 1', addressBase: `/codec/tac5212/dac/${n}/bq/1`,
      defaults: EQ_PRESETS.smooth[0] },
    { kind: 'biquad', label: 'Band 2', addressBase: `/codec/tac5212/dac/${n}/bq/2`,
      defaults: EQ_PRESETS.smooth[1] },
    { kind: 'biquad', label: 'Band 3', addressBase: `/codec/tac5212/dac/${n}/bq/3`,
      defaults: EQ_PRESETS.smooth[2] },
  ],
});

const adcChannelSection = (n: 1 | 2): Section => ({
  name: `Channel ${n}`,
  collapsible: true,
  controls: [
    { kind: 'range', label: 'Digital gain', address: `/codec/tac5212/adc/${n}/dvol`,
      min: -12, max: 27, step: 0.5, unit: 'dB',
      help: 'Per-channel digital gain after the converter.' },
    { kind: 'enum', label: 'Mode', address: `/codec/tac5212/adc/${n}/mode`,
      options: ['differential', 'single_ended_inp'],
      help: 'Differential rejects board noise; single-ended uses one pin.' },
    { kind: 'enum', label: 'Impedance', address: `/codec/tac5212/adc/${n}/impedance`,
      options: ['5k', '10k', '40k'],
      help: 'Higher impedance loads the source less; lower has less noise.' },
    { kind: 'enum', label: 'Full scale', address: `/codec/tac5212/adc/${n}/fullscale`,
      options: ['2vrms', '4vrms'] },
    { kind: 'enum', label: 'Coupling', address: `/codec/tac5212/adc/${n}/coupling`,
      options: ['ac', 'dc_low', 'dc_rail_to_rail'] },
    { kind: 'enum', label: 'Bandwidth', address: `/codec/tac5212/adc/${n}/bw`,
      options: ['24khz', '96khz'] },
  ],
});

// ---------------------------------------------------------------------------
// The Tab[] tree — fed straight to codec-panel.ts to render.
// ---------------------------------------------------------------------------

export const tac5212Panel: Tab[] = [
  {
    name: 'DAC EQ',
    sections: [
      {
        name: 'Output link',
        subtitle: 'Edit Output 1; Output 2 mirrors. Per-output tweaks need this off.',
        controls: [
          { kind: 'client_toggle', label: 'Link outputs',
            signalKey: 'eq_link',
            disableSection: 'Output 2 — Tone shaping',
            help: 'When on, Output 1 sliders push the same design to Output 2.' },
        ],
      },
      dacEqSection(1),
      dacEqSection(2),
      {
        name: 'EQ allocation (chip-wide)',
        subtitle: 'How many EQ bands per output. More bands = more shaping.',
        controls: [
          { kind: 'range', label: 'Bands per channel',
            address: '/codec/tac5212/dac/biquads',
            min: 0, max: 3, step: 1, unit: 'bands' },
        ],
      },
    ],
  },

  {
    name: 'DAC Dynamics',
    sections: [
      {
        name: 'DRC (compressor) — chip-wide',
        subtitle: 'Tames loud transients to reduce ear fatigue. Pre-tuned coefficients.',
        controls: [
          { kind: 'toggle', label: 'Enable', address: '/codec/tac5212/dac/drc/enable',
            help: 'Applies to all DAC channels. Default coefficients are loaded automatically when enabled.' },
          { kind: 'action', label: 'Reload defaults', address: '/codec/tac5212/dac/drc/preset' },
        ],
      },
      {
        name: 'Distortion limiter — chip-wide',
        subtitle: 'Soft-clip ceiling that catches harsh peak transients before they hit the DAC.',
        disabledReason:
          'Requires PPC3-generated coefficients. The chip\'s POR coefficient values produce ' +
          'audible self-oscillation when enabled with arbitrary audio, and the datasheet ' +
          'does not publish the encoding for hand-calculated values — TI directs users to ' +
          'the PurePath Console (PPC3) GUI. Re-enable once a PPC3-derived preset is wired in.',
        controls: [
          { kind: 'toggle', label: 'Enable', address: '/codec/tac5212/dac/limiter/enable',
            help: 'Applied to both outputs. Default coefficients are loaded automatically when enabled.' },
          { kind: 'enum', label: 'Detection', address: '/codec/tac5212/dac/limiter/input',
            options: ['max', 'avg'],
            help: 'max = react to the louder channel (defensive). avg = follow averaged level (gentler).' },
          { kind: 'action', label: 'Reload defaults', address: '/codec/tac5212/dac/limiter/preset' },
        ],
      },
    ],
  },

  {
    name: 'DAC Filter & Volume',
    sections: [
      {
        name: 'Digital volume',
        subtitle: 'Per-output digital volume. -100 dB to +27 dB, 0.5 dB steps.',
        controls: [
          { kind: 'range', label: 'Output 1', address: '/codec/tac5212/dac/1/dvol',
            min: -60, max: 12, step: 0.5, unit: 'dB' },
          { kind: 'range', label: 'Output 2', address: '/codec/tac5212/dac/2/dvol',
            min: -60, max: 12, step: 0.5, unit: 'dB' },
          { kind: 'toggle', label: 'Gang outputs', address: '/codec/tac5212/dac/dvol_gang',
            help: 'When on, output 1 controls both channels.' },
          { kind: 'toggle', label: 'Soft-step on changes', address: '/codec/tac5212/dac/soft_step',
            help: 'Smooth volume transitions instead of stepping.' },
        ],
      },
      {
        name: 'Interpolation filter (chip-wide)',
        subtitle: 'How the DAC reconstructs samples. Affects transient feel.',
        controls: [
          { kind: 'enum', label: 'Mode', address: '/codec/tac5212/dac/interp',
            options: ['linear_phase', 'low_latency', 'ultra_low_latency', 'low_power'],
            help: 'Linear phase = best tone. Low-latency = best for live monitoring.' },
        ],
      },
      {
        name: 'DAC high-pass filter (chip-wide)',
        subtitle: 'Removes DC and infrasonic content from playback.',
        controls: [
          { kind: 'enum', label: 'Cutoff', address: '/codec/tac5212/dac/hpf',
            options: ['off', '1hz', '12hz', '96hz'] },
        ],
      },
    ],
  },

  {
    name: 'ADC',
    sections: [
      {
        name: 'Line input',
        controls: [
          { kind: 'enum', label: 'Mode', address: '/line/mode', options: ['stereo', 'mono'],
            disableSection: { value: 'mono', sectionName: 'Channel 2' } },
        ],
      },
      adcChannelSection(1),
      adcChannelSection(2),
      {
        name: 'ADC high-pass filter (chip-wide)',
        subtitle: 'Removes mic rumble and DC offset.',
        controls: [
          { kind: 'toggle', label: 'Enable', address: '/codec/tac5212/adc/hpf' },
        ],
      },
    ],
  },

  {
    name: 'Routing & Reference',
    sections: [
      {
        name: 'Output drivers',
        controls: [
          { kind: 'enum', label: 'Output 1 mode', address: '/codec/tac5212/out/1/mode',
            options: ['diff_line', 'se_line', 'hp_driver', 'receiver'] },
          { kind: 'enum', label: 'Output 2 mode', address: '/codec/tac5212/out/2/mode',
            options: ['diff_line', 'se_line', 'hp_driver', 'receiver'] },
        ],
      },
      {
        name: 'Voltage reference',
        controls: [
          { kind: 'enum', label: 'VREF full scale', address: '/codec/tac5212/vref/fscale',
            options: ['2.75v', '2.5v', '1.375v', '1.25v', 'avdd'] },
        ],
      },
      {
        name: 'Mic bias',
        controls: [
          { kind: 'toggle', label: 'Enable', address: '/codec/tac5212/micbias/enable' },
          { kind: 'enum', label: 'Level', address: '/codec/tac5212/micbias/level',
            options: ['2.75v', '2.5v', '1.375v', '1.25v', 'avdd'] },
        ],
      },
      {
        name: 'PDM mic',
        collapsible: true,
        defaultCollapsed: true,
        controls: [
          { kind: 'toggle', label: 'Enable', address: '/codec/tac5212/pdm/enable' },
          { kind: 'enum', label: 'Source', address: '/codec/tac5212/pdm/source',
            options: ['gpi1', 'gpi2'] },
        ],
      },
    ],
  },

  {
    name: 'System',
    sections: [
      {
        name: 'Device',
        controls: [
          { kind: 'action', label: 'Reset',      address: '/codec/tac5212/reset',
            help: 'SW_RESET only — leaves the chip in raw POR state (silent). Use Initialize to bring audio back.' },
          { kind: 'action', label: 'Initialize', address: '/codec/tac5212/init',
            help: 'Re-runs the full boot bring-up: reset, serial format, slot mapping, ADC/DAC config, power-up, unmute. Recovers from a stuck state without a power cycle.' },
          { kind: 'radio',  label: 'Output',
            help: 'Headphone = 16Ω driver on OUT1/OUT2 jack (TRS). Line Out = single-ended line driver, signal on tip referenced to sleeve — feed an external amp or RCA via a 3.5mm-RCA pigtail. Sets both OUT1 and OUT2.',
            options: [
              { label: 'Headphone', writes: [
                { address: '/codec/tac5212/out/1/mode', value: 'hp_driver' },
                { address: '/codec/tac5212/out/2/mode', value: 'hp_driver' },
              ]},
              { label: 'Line Out', writes: [
                { address: '/codec/tac5212/out/1/mode', value: 'se_line' },
                { address: '/codec/tac5212/out/2/mode', value: 'se_line' },
              ]},
            ] },
          { kind: 'toggle', label: 'Wake',       address: '/codec/tac5212/wake' },
          { kind: 'action', label: 'Info',       address: '/codec/tac5212/info' },
          { kind: 'action', label: 'Status',     address: '/codec/tac5212/status' },
        ],
      },
    ],
  },
];
