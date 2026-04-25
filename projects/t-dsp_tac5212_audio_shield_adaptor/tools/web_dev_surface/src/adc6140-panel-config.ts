// Declarative descriptor for the TLV320ADC6140 codec settings panel.
//
// Mirrors codec-panel-config.ts (TAC5212) — same Tab/Section/Control shape so
// the existing renderer in ui/codec-panel.ts can paint it. Address tree
// matches Adc6140Panel::route() in src/Adc6140Panel.cpp.
//
// Hierarchy:
//   Tabs (top-level sections) → Sections (section headers) → Controls (sub-settings)
//
// Tabs:
//   - Channels        per-channel front-end + gain (one section per ch)
//   - Reference       chip-global VREF / full-scale / MICBIAS
//   - Signal Chain    HPF / decimation / channel-sum
//   - Dynamic Range   DRE vs AGC mode select + their parameters
//   - System          info / status / raw register access

import type { Tab, Section } from './codec-panel-config';

const xlrChannel = (n: number): Section => ({
  name: `XLR Channel ${n}`,
  controls: [
    {
      kind: 'range',
      label: 'Analog gain (preamp)',
      address: `/codec/adc6140/ch/${n}/gain`,
      min: 0,
      max: 42,
      step: 1,
      unit: 'dB',
    },
    {
      kind: 'range',
      label: 'Digital volume',
      address: `/codec/adc6140/ch/${n}/dvol`,
      min: -60,
      max: 27,
      step: 0.5,
      unit: 'dB',
    },
    {
      kind: 'enum',
      label: 'Input source',
      address: `/codec/adc6140/ch/${n}/source`,
      options: ['differential', 'single_ended'],
    },
    {
      kind: 'enum',
      label: 'Input type',
      address: `/codec/adc6140/ch/${n}/intype`,
      options: ['mic', 'line'],
    },
    {
      kind: 'enum',
      label: 'Coupling',
      address: `/codec/adc6140/ch/${n}/coupling`,
      options: ['ac', 'dc'],
    },
    {
      kind: 'enum',
      label: 'Input impedance',
      address: `/codec/adc6140/ch/${n}/impedance`,
      options: ['2.5k', '10k', '20k'],
    },
    {
      kind: 'toggle',
      label: 'DRE / AGC enable',
      address: `/codec/adc6140/ch/${n}/dre`,
    },
    {
      kind: 'range',
      label: 'Gain trim (calibration)',
      address: `/codec/adc6140/ch/${n}/gaincal`,
      min: -0.8,
      max: 0.7,
      step: 0.1,
      unit: 'dB',
    },
  ],
});

export const adc6140Panel: Tab[] = [
  {
    name: 'Channels',
    sections: [
      xlrChannel(1),
      xlrChannel(2),
      xlrChannel(3),
      xlrChannel(4),
    ],
  },
  {
    name: 'Reference',
    sections: [
      {
        name: 'ADC full-scale',
        controls: [
          {
            kind: 'enum',
            label: 'VREF / FS',
            address: '/codec/adc6140/fullscale',
            options: ['2v75', '2v5', '1v375'],
          },
        ],
      },
      {
        name: 'MICBIAS',
        controls: [
          {
            kind: 'enum',
            label: 'Output level',
            address: '/codec/adc6140/micbias',
            options: ['off', 'vref', 'vref_boosted', 'avdd'],
          },
        ],
      },
    ],
  },
  {
    name: 'Signal Chain',
    sections: [
      {
        name: 'High-pass filter',
        controls: [
          {
            kind: 'enum',
            label: 'Cutoff (× fs)',
            address: '/codec/adc6140/hpf',
            options: ['programmable', '12hz', '96hz', '384hz'],
          },
        ],
      },
      {
        name: 'Decimation filter',
        controls: [
          {
            kind: 'enum',
            label: 'Phase / latency',
            address: '/codec/adc6140/decimfilt',
            options: ['linear', 'low_latency', 'ultra_low_latency'],
          },
        ],
      },
      {
        name: 'Channel summing',
        controls: [
          {
            kind: 'enum',
            label: 'Mode',
            address: '/codec/adc6140/chsum',
            options: ['off', 'pairs', 'quad'],
          },
        ],
      },
    ],
  },
  {
    name: 'Dynamic Range',
    sections: [
      {
        name: 'Mode',
        controls: [
          {
            kind: 'enum',
            label: 'Algorithm',
            address: '/codec/adc6140/mode',
            options: ['dre', 'agc'],
          },
        ],
      },
      {
        name: 'DRE (dynamic range enhancer)',
        controls: [
          {
            kind: 'range',
            label: 'Trigger level',
            address: '/codec/adc6140/dre/level',
            min: -66,
            max: -12,
            step: 6,
            unit: 'dBFS',
          },
          {
            kind: 'range',
            label: 'Maximum gain',
            address: '/codec/adc6140/dre/maxgain',
            min: 2,
            max: 30,
            step: 2,
            unit: 'dB',
          },
        ],
      },
      {
        name: 'AGC (automatic gain control)',
        controls: [
          {
            kind: 'range',
            label: 'Target output level',
            address: '/codec/adc6140/agc/target',
            min: -36,
            max: -6,
            step: 2,
            unit: 'dBFS',
          },
          {
            kind: 'range',
            label: 'Maximum gain',
            address: '/codec/adc6140/agc/maxgain',
            min: 3,
            max: 42,
            step: 3,
            unit: 'dB',
          },
        ],
      },
    ],
  },
  {
    name: 'System',
    sections: [
      {
        name: 'Chip',
        controls: [
          { kind: 'action', label: 'Info',   address: '/codec/adc6140/info' },
          { kind: 'action', label: 'Status', address: '/codec/adc6140/status' },
        ],
      },
    ],
  },
];
