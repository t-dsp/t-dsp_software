// TUNE workspace stub — Solid + Tailwind port (Phase 2 spike).
//
// First Solid component in the codebase. Same content as the vanilla
// stub built in Phase 1; this version proves the toolchain end-to-end:
//   - vite-plugin-solid compiles the JSX
//   - tailwindcss generates utilities from src/tailwind.css @theme
//   - signal-bridge adapts state.selectedChannel to a Solid Accessor
//   - render() mounts the component into a host HTMLElement so the
//     existing main.ts wiring (which expects an HTMLElement back from
//     each panel builder) keeps working unchanged.
//
// When the firmware exposes per-channel HPF / EQ / Dynamics this is
// the file that grows into the real TUNE workspace; for Phase 2 it
// stays a stub but lights up the new toolchain.

import { For, type Component } from 'solid-js';
import { render } from 'solid-js/web';
import type { MixerState } from '../state';
import { asSolid } from '../signal-bridge';

interface Props {
  state: MixerState;
}

const PENDING: ReadonlyArray<readonly [string, string]> = [
  ['HPF',           'pending firmware support'],
  ['Parametric EQ', 'pending firmware support'],
  ['Dynamics',      'pending firmware support'],
  ['Pan',           'addressable today; widget pending'],
  ['Sends matrix',  'partial — Main only until aux buses land'],
] as const;

const TuneStub: Component<Props> = (props) => {
  // Sel state — global signal, driven by Sel buttons in the MIX strips
  // and the bottom-strip mini-bank. Solid auto-tracks reads inside JSX.
  const sel = asSolid(props.state.selectedChannel);

  // Resolve sel idx → name. The channel name is itself a signal
  // (firmware can rename via /ch/NN/config/name); we wrap each one and
  // pick by current sel index. Memoizing per-index is overkill for 10
  // channels — derive on render.
  const channelNames = props.state.channels.map((ch) => asSolid(ch.name));
  const selName = () => channelNames[sel()]?.() ?? `Ch ${sel() + 1}`;

  return (
    <div class="bg-surface-1 border border-surface-2 rounded p-6 flex flex-col gap-4">
      <header class="flex flex-col gap-1">
        <h2 class="m-0 text-[18px] font-medium text-fg-1 tracking-wide">
          TUNE — selected channel detail
        </h2>
        <div class="text-sm text-accent-500 font-mono">
          Selected: {selName()} (idx {sel()})
        </div>
      </header>

      <p class="m-0 text-sm text-fg-2 leading-relaxed max-w-[720px]">
        This workspace will host per-channel HPF, parametric EQ,
        dynamics, pan, and a sends matrix for whichever channel is
        Sel’d. Most of those controls are blocked on firmware exposing
        per-channel processing — see{' '}
        <code class="font-mono bg-surface-0 px-1.5 py-0.5 rounded border border-surface-2 text-[13px]">
          planning/ui-rebuild/05-roadmap.md
        </code>{' '}
        Phase 5.
      </p>

      <ul class="list-none p-0 m-0 flex flex-col gap-2">
        <For each={PENDING}>{([name, status]) => (
          <li class="text-[13px] text-fg-2 px-3 py-2 bg-surface-0 border border-surface-2 rounded">
            <strong class="text-fg-1 font-medium">{name}: </strong>
            <span>{status}</span>
          </li>
        )}</For>
      </ul>
    </div>
  );
};

// Mount helper so main.ts can stay agnostic of Solid: returns an
// HTMLElement that contains the rendered component. Same signature as
// every other panel builder in ui/.
export function tuneStubPanel(state: MixerState): HTMLElement {
  const root = document.createElement('section');
  root.className = 'view view-tune';
  render(() => <TuneStub state={state} />, root);
  return root;
}
