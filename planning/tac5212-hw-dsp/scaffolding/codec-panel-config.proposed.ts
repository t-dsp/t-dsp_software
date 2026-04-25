// Proposed extension of codec-panel-config.ts.
//
// Scaffolding only. When this branch ships, this file gets folded into the
// existing tools/web_dev_surface/src/codec-panel-config.ts so there's one
// declarative source of truth. Kept separate here for review.
//
// Renames `Group` to `Section` (existing places that reference Group get
// migrated), adds `subtitle?`, `collapsible?`, `defaultCollapsed?`, and
// `presets?` to support the hierarchical UX. Adds three new control kinds:
// `biquad`, `drc`, `limiter`. Adds optional `help?` to all controls.

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
  | { kind: 'range'; label: string; help?: string; address: string; min: number; max: number; step: number; unit: string }
  // NEW — multi-control composites:
  | { kind: 'biquad'; label: string; help?: string;
      addressBase: string;       // e.g. /codec/tac5212/dac/1/bq/2 (band 2 of output 1)
      defaults: BiquadDesign }
  | { kind: 'drc'; label: string; help?: string;
      addressBase: string;       // e.g. /codec/tac5212/dac/1/drc
      defaults: { thresholdDb: number; ratio: number; attackMs: number; releaseMs: number; maxGainDb: number; kneeDb: number } }
  | { kind: 'limiter'; label: string; help?: string;
      addressBase: string;       // e.g. /codec/tac5212/dac/1/limiter
      defaults: { thresholdDb: number; attackMs: number; releaseMs: number; kneeDb: number } };

export interface PresetButton {
  label: string;
  // Each preset emits a named action address; firmware expands to the
  // corresponding parameter writes. Keeps the wire format simple.
  // Address example: /codec/tac5212/dac/1/eq/preset s "smooth"
  address: string;
  arg: string;
}

export interface Section {
  name: string;
  subtitle?: string;
  collapsible?: boolean;
  defaultCollapsed?: boolean;
  presets?: PresetButton[];
  controls: Control[];
}

export interface Tab {
  name: string;
  sections: Section[];     // renamed from `groups`
}

// ---------------------------------------------------------------------------
// Helpers — section builders for repeated patterns.
// ---------------------------------------------------------------------------

const dacEqSection = (n: 1 | 2): Section => ({
  name: `Output ${n} — Tone shaping`,
  subtitle: '3 hardware EQ bands. Cut harshness, add warmth.',
  collapsible: true,
  presets: [
    { label: 'Flat',   address: `/codec/tac5212/dac/${n}/eq/preset`, arg: 'flat' },
    { label: 'Smooth', address: `/codec/tac5212/dac/${n}/eq/preset`, arg: 'smooth' },
    { label: 'Bright', address: `/codec/tac5212/dac/${n}/eq/preset`, arg: 'bright' },
    { label: 'Dark',   address: `/codec/tac5212/dac/${n}/eq/preset`, arg: 'dark' },
  ],
  controls: [
    { kind: 'biquad', label: 'Band 1', addressBase: `/codec/tac5212/dac/${n}/bq/1`,
      defaults: { type: 'low_shelf',  freqHz: 100,  gainDb: 0, q: 0.7 } },
    { kind: 'biquad', label: 'Band 2', addressBase: `/codec/tac5212/dac/${n}/bq/2`,
      defaults: { type: 'peak',       freqHz: 1000, gainDb: 0, q: 1.0 } },
    { kind: 'biquad', label: 'Band 3', addressBase: `/codec/tac5212/dac/${n}/bq/3`,
      defaults: { type: 'high_shelf', freqHz: 8000, gainDb: 0, q: 0.7 } },
  ],
});

const dacDrcSection = (n: 1 | 2): Section => ({
  name: `Output ${n} — DRC (compressor)`,
  subtitle: 'Tames loud transients smoothly. The "ear-fatigue fix".',
  collapsible: true,
  controls: [
    { kind: 'toggle', label: 'Enable',  address: `/codec/tac5212/dac/${n}/drc/enable` },
    { kind: 'drc', label: 'Settings',
      addressBase: `/codec/tac5212/dac/${n}/drc`,
      defaults: { thresholdDb: -20, ratio: 2, attackMs: 10, releaseMs: 200, maxGainDb: 12, kneeDb: 6 } },
  ],
});

const dacLimiterSection = (n: 1 | 2): Section => ({
  name: `Output ${n} — Limiter (peak protection)`,
  subtitle: 'Hard ceiling that prevents clipping at any cost.',
  collapsible: true,
  defaultCollapsed: true,
  controls: [
    { kind: 'toggle', label: 'Enable', address: `/codec/tac5212/dac/${n}/limiter/enable` },
    { kind: 'limiter', label: 'Settings',
      addressBase: `/codec/tac5212/dac/${n}/limiter`,
      defaults: { thresholdDb: -1, attackMs: 0.1, releaseMs: 50, kneeDb: 2 } },
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
      help: 'Higher impedance loads the source less; lower has lower noise.' },
    { kind: 'enum', label: 'Full scale', address: `/codec/tac5212/adc/${n}/fullscale`,
      options: ['2vrms', '4vrms'] },
    { kind: 'enum', label: 'Coupling', address: `/codec/tac5212/adc/${n}/coupling`,
      options: ['ac', 'dc_low', 'dc_rail_to_rail'] },
    { kind: 'enum', label: 'Bandwidth', address: `/codec/tac5212/adc/${n}/bw`,
      options: ['24khz', '96khz'] },
  ],
});

// ---------------------------------------------------------------------------
// The proposed Tab[] — fed straight to codec-panel.ts to render.
// ---------------------------------------------------------------------------

export const tac5212Panel: Tab[] = [
  {
    name: 'Signal Chain',
    sections: [
      {
        name: 'Audio path overview',
        subtitle: 'Click any block below to jump to its sub-tab.',
        // Special section: rendered as a flow diagram, not a control grid.
        // Implementation: codec-panel.ts checks for an empty controls[] +
        // a `flowDiagram?: true` flag (added later) to render the SVG
        // pipeline. For v1 this is just a banner panel with text.
        controls: [],
      },
    ],
  },

  {
    name: 'DAC EQ',
    sections: [
      dacEqSection(1),
      dacEqSection(2),
      {
        name: 'EQ allocation (chip-wide)',
        subtitle: 'How many EQ bands per output. Trades flexibility for CPU.',
        controls: [
          { kind: 'enum', label: 'Bands per channel',
            address: '/codec/tac5212/dac/biquads',
            options: ['0', '1', '2', '3'] },
        ],
      },
    ],
  },

  {
    name: 'Dynamics',
    sections: [
      dacDrcSection(1),
      dacLimiterSection(1),
      dacDrcSection(2),
      dacLimiterSection(2),
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
            help: 'Linear phase is best for tone; low-latency is best for live monitoring.' },
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
          { kind: 'enum', label: 'Cutoff', address: '/codec/tac5212/adc/hpf',
            options: ['off', '1hz', '12hz', '96hz'] },
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
          { kind: 'action', label: 'Reset',  address: '/codec/tac5212/reset' },
          { kind: 'toggle', label: 'Wake',   address: '/codec/tac5212/wake' },
          { kind: 'action', label: 'Info',   address: '/codec/tac5212/info' },
          { kind: 'action', label: 'Status', address: '/codec/tac5212/status' },
        ],
      },
      // Diagnostics: raw register set/get is wired through the existing
      // raw-osc pane in the Mixer tab; no UI here, but the firmware leaves
      // /codec/tac5212/reg/{set,get} exposed.
    ],
  },
];
