// Declarative descriptor for the TAC5212 codec settings panel.
//
// The leaves and enum options here mirror the canonical /codec/tac5212/...
// address tree from planning/osc-mixer-foundation/02-osc-protocol.md (the
// "TAC5212 codec panel design notes" section). When that subtree changes,
// update this file in lockstep — there is no other source of truth in the
// browser surface for codec leaves.
//
// Speculative leaves explicitly removed in the design notes (/out/N/level,
// /out/N/drive, "se_mic_bias", /adc/N/pga, /micbias/gpio_control, /bypass/...)
// are NOT in this descriptor and should not be added without revisiting the
// rules in 02-osc-protocol.md.

export type Control =
  | { kind: 'enum'; label: string; address: string; options: string[] }
  | { kind: 'toggle'; label: string; address: string }
  | { kind: 'action'; label: string; address: string };

export interface Group {
  name: string;
  controls: Control[];
}

export interface Tab {
  name: string;
  groups: Group[];
}

const adcChannel = (n: number): Group => ({
  name: `Channel ${n}`,
  controls: [
    {
      kind: 'enum',
      label: 'Mode',
      address: `/codec/tac5212/adc/${n}/mode`,
      options: ['single_ended_inp', 'differential'],
    },
    {
      kind: 'enum',
      label: 'Impedance',
      address: `/codec/tac5212/adc/${n}/impedance`,
      options: ['5k', '10k', '40k'],
    },
    {
      kind: 'enum',
      label: 'Full scale',
      address: `/codec/tac5212/adc/${n}/fullscale`,
      options: ['2vrms', '4vrms'],
    },
    {
      kind: 'enum',
      label: 'Coupling',
      address: `/codec/tac5212/adc/${n}/coupling`,
      options: ['ac', 'dc_low', 'dc_rail_to_rail'],
    },
    {
      kind: 'enum',
      label: 'Bandwidth',
      address: `/codec/tac5212/adc/${n}/bw`,
      options: ['24khz', '96khz'],
    },
  ],
});

const outputDriver = (n: number): Group => ({
  name: `Output ${n}`,
  controls: [
    {
      kind: 'enum',
      label: 'Mode',
      address: `/codec/tac5212/out/${n}/mode`,
      options: ['diff_line', 'se_line', 'hp_driver', 'receiver'],
    },
  ],
});

export const tac5212Panel: Tab[] = [
  {
    name: 'ADC',
    groups: [
      adcChannel(1),
      adcChannel(2),
      {
        name: 'Global',
        controls: [
          {
            kind: 'toggle',
            label: 'HPF (chip-global)',
            address: '/codec/tac5212/adc/hpf',
          },
        ],
      },
    ],
  },
  {
    name: 'VREF / MICBIAS',
    groups: [
      {
        name: 'VREF',
        controls: [
          {
            kind: 'enum',
            label: 'Full scale',
            address: '/codec/tac5212/vref/fscale',
            options: ['2.75v', '2.5v', '1.375v', '1.25v', 'avdd'],
          },
        ],
      },
      {
        name: 'MICBIAS',
        controls: [
          {
            kind: 'toggle',
            label: 'Enable',
            address: '/codec/tac5212/micbias/enable',
          },
          {
            kind: 'enum',
            label: 'Level',
            address: '/codec/tac5212/micbias/level',
            options: ['2.75v', '2.5v', '1.375v', '1.25v', 'avdd'],
          },
        ],
      },
    ],
  },
  {
    name: 'Output',
    groups: [outputDriver(1), outputDriver(2)],
  },
  {
    name: 'PDM',
    groups: [
      {
        name: 'PDM mic',
        controls: [
          {
            kind: 'toggle',
            label: 'Enable',
            address: '/codec/tac5212/pdm/enable',
          },
          {
            kind: 'enum',
            label: 'Source',
            address: '/codec/tac5212/pdm/source',
            options: ['gpi1', 'gpi2'],
          },
        ],
      },
    ],
  },
  {
    name: 'System',
    groups: [
      {
        name: 'Chip',
        controls: [
          { kind: 'action', label: 'Reset', address: '/codec/tac5212/reset' },
          { kind: 'toggle', label: 'Wake', address: '/codec/tac5212/wake' },
          { kind: 'action', label: 'Info', address: '/codec/tac5212/info' },
          { kind: 'action', label: 'Status', address: '/codec/tac5212/status' },
        ],
      },
    ],
  },
];
