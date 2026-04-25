// Codec panel — declarative descriptor (codec-panel-config.ts) -> reactive UI.
//
// Hierarchy (matches planning/tac5212-hw-dsp/PLAN.md §5):
//   Tab → Section card → Sub-setting row
//
// Each section is a bordered card with bold uppercase header, optional
// subtitle, optional preset buttons, and optional collapse/expand. Controls
// inside a section render in a 2-column grid (label : control) with
// per-control help text under the label.
//
// The composite `biquad` control kind delegates to ui/biquad-widget.ts for
// rendering type select + freq/gain/Q sliders + a shared response-curve
// canvas at the section level.
//
// Tab switching toggles CSS display rather than destroying nodes — keeps
// dispatcher listeners stable across switches.

import { Tab, Control, Section, PresetButton } from '../codec-panel-config';
import { Dispatcher } from '../dispatcher';
import { Signal } from '../state';
import { OscMessage } from '../osc';
import { biquadBand, eqCurve, BiquadBandHandle, EqCurveHandle } from './biquad-widget';
import { eqLinkSignal } from '../eq-link';

export function codecPanel(tabs: Tab[], dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'codec-panel';

  const tabBar = document.createElement('div');
  tabBar.className = 'tab-bar';

  const content = document.createElement('div');
  content.className = 'tab-content';

  const tabPanels: HTMLElement[] = [];
  const tabButtons: HTMLButtonElement[] = [];

  // Track section elements by name so controls can disable/enable sections.
  const sectionElements = new Map<string, HTMLElement>();

  let activeIdx = 0;

  function selectTab(i: number): void {
    activeIdx = i;
    tabButtons.forEach((b, j) => b.classList.toggle('active', j === i));
    tabPanels.forEach((p, j) => {
      p.style.display = j === i ? '' : 'none';
    });
  }

  tabs.forEach((tab, i) => {
    const btn = document.createElement('button');
    btn.className = 'tab-btn';
    btn.textContent = tab.name;
    btn.addEventListener('click', () => selectTab(i));
    tabBar.appendChild(btn);
    tabButtons.push(btn);

    const panel = document.createElement('div');
    panel.className = 'tab-panel';
    for (const section of tab.sections) {
      const sectionEl = renderSection(section, dispatcher, sectionElements);
      panel.appendChild(sectionEl);
      sectionElements.set(section.name, sectionEl);
    }
    content.appendChild(panel);
    tabPanels.push(panel);
  });

  selectTab(0);
  root.append(tabBar, content);
  return root;
}

// ---------------------------------------------------------------------------
// Section rendering
// ---------------------------------------------------------------------------

function renderSection(
  section: Section,
  dispatcher: Dispatcher,
  sectionElements: Map<string, HTMLElement>,
): HTMLElement {
  const card = document.createElement('div');
  card.className = 'codec-section-card';
  if (section.disabledReason) {
    card.classList.add('codec-section-card-disabled');
    card.title = section.disabledReason;
  }

  // --- header ---
  const header = document.createElement('div');
  header.className = 'codec-section-header';

  const titleSide = document.createElement('div');
  titleSide.className = 'title';
  if (section.collapsible) {
    const chev = document.createElement('span');
    chev.className = 'chevron';
    chev.textContent = '▾';
    titleSide.appendChild(chev);
  }
  const title = document.createElement('span');
  title.textContent = section.name;
  titleSide.appendChild(title);
  if (section.subtitle) {
    const sub = document.createElement('span');
    sub.className = 'codec-section-subtitle';
    sub.textContent = section.subtitle;
    titleSide.appendChild(sub);
  }
  header.appendChild(titleSide);

  // --- preset row (right side of header) ---
  // Container is created up-front so the layout is right, but buttons are
  // appended at the end of renderSection — they need band handles to wire
  // setDesign() into preset clicks.
  let presetRow: HTMLElement | null = null;
  if (section.presets && section.presets.length > 0) {
    presetRow = document.createElement('div');
    presetRow.className = 'codec-section-presets';
    header.appendChild(presetRow);
  }
  card.appendChild(header);

  // --- body ---
  const body = document.createElement('div');
  body.className = 'codec-section-body';
  if (section.disabledReason) {
    // `inert` removes from tab order and disables clicks/focus inside the
    // body without disabling each control individually. Supported in all
    // current evergreen browsers and the Electron version we ship.
    body.inert = true;
  }

  // Section can host a shared EQ-curve canvas if any of its controls are
  // biquad bands. Build it lazily.
  let curve: EqCurveHandle | null = null;
  const bands: BiquadBandHandle[] = [];

  for (const ctl of section.controls) {
    if (ctl.kind === 'biquad') {
      if (!curve) {
        curve = eqCurve();
        // Curve sits at the top of the body, spanning both columns.
        const curveRow = document.createElement('div');
        curveRow.className = 'codec-curve-row';
        curveRow.appendChild(curve.element);
        body.appendChild(curveRow);
      }
      const band = biquadBand(ctl.label, ctl.addressBase, ctl.defaults, dispatcher,
        () => curve?.repaint());
      bands.push(band);
      // Each band gets a labeled row spanning both grid columns.
      const row = document.createElement('div');
      row.className = 'codec-biquad-row';
      row.appendChild(band.element);
      body.appendChild(row);
      if (ctl.help) {
        const help = document.createElement('div');
        help.className = 'codec-control-help';
        help.textContent = ctl.help;
        body.appendChild(help);
      }
    } else {
      // Standard 2-column row.
      const labelEl = document.createElement('div');
      labelEl.className = 'codec-row-label';
      labelEl.textContent = ctl.label;
      body.appendChild(labelEl);

      const controlEl = document.createElement('div');
      controlEl.className = 'codec-row-control';
      controlEl.appendChild(renderSimpleControl(ctl, dispatcher, sectionElements));
      body.appendChild(controlEl);

      if (ctl.help) {
        const help = document.createElement('div');
        help.className = 'codec-control-help';
        help.textContent = ctl.help;
        body.appendChild(help);
      }
    }
  }
  card.appendChild(body);

  if (curve) {
    curve.setBands(bands);
  }

  // Now that bands exist, wire preset buttons.
  if (presetRow && section.presets) {
    const bandsByAddress = new Map<string, BiquadBandHandle>();
    for (const b of bands) bandsByAddress.set(b.addressBase, b);
    for (const p of section.presets) {
      presetRow.appendChild(renderPresetButton(p, dispatcher, bandsByAddress));
    }
  }

  // --- collapsible toggle ---
  if (section.collapsible) {
    const start = section.defaultCollapsed === true;
    if (start) {
      body.style.display = 'none';
      header.classList.add('collapsed');
    }
    header.addEventListener('click', () => {
      const collapsed = body.style.display === 'none';
      body.style.display = collapsed ? '' : 'none';
      header.classList.toggle('collapsed', !collapsed);
    });
  }

  return card;
}

function renderPresetButton(
  p: PresetButton,
  dispatcher: Dispatcher,
  bandsByAddress: Map<string, BiquadBandHandle>,
): HTMLButtonElement {
  const btn = document.createElement('button');
  btn.className = 'codec-preset-btn';
  btn.type = 'button';
  btn.textContent = p.label;
  btn.addEventListener('click', (ev) => {
    // Stop the click from bubbling into the section header (which also
    // listens for click → toggle collapse).
    ev.stopPropagation();
    if (p.designs && p.designs.length > 0) {
      // Drive the band's own setDesign() — same code path a slider
      // gesture takes, so sliders/readouts/coefs/curve all update
      // immediately. The widget also ships /design to firmware (and
      // mirrors to /dac/2/ when link is on, via biquad-widget.ts).
      for (const d of p.designs) {
        const band = bandsByAddress.get(d.addressBase);
        if (band) band.setDesign(d.design);
      }
    } else if (p.address !== undefined) {
      dispatcher.sendRaw(p.address, 's', [p.arg ?? '']);
    }
  });
  return btn;
}

// ---------------------------------------------------------------------------
// Simple controls (everything except `biquad`)
// ---------------------------------------------------------------------------

function renderSimpleControl(
  ctl: Exclude<Control, { kind: 'biquad' }>,
  dispatcher: Dispatcher,
  sectionElements: Map<string, HTMLElement>,
): HTMLElement {
  const wrap = document.createElement('div');
  wrap.className = 'control';

  if (ctl.kind === 'enum') {
    const value = new Signal<string | null>(null);

    const sel = document.createElement('select');
    for (const opt of ctl.options) {
      const o = document.createElement('option');
      o.value = opt;
      o.textContent = opt;
      sel.appendChild(o);
    }
    sel.addEventListener('change', () => {
      value.set(sel.value);
      dispatcher.sendRaw(ctl.address, 's', [sel.value]);
    });

    // disableSection: when the selected value matches, grey out the named
    // section. Used by /line/mode "mono" → disable "Channel 2".
    const ds = ctl.disableSection;
    value.subscribe((v) => {
      if (v !== null && sel.value !== v) sel.value = v;
      if (ds) {
        const sec = sectionElements.get(ds.sectionName);
        if (sec) sec.classList.toggle('section-disabled', v === ds.value);
      }
    });

    dispatcher.registerCodecListener(ctl.address, (msg: OscMessage) => {
      if (msg.types === 's' && typeof msg.args[0] === 'string') {
        value.set(msg.args[0]);
      }
    });

    wrap.appendChild(sel);
  } else if (ctl.kind === 'toggle') {
    const value = new Signal<boolean | null>(null);

    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.addEventListener('change', () => {
      value.set(cb.checked);
      dispatcher.sendRaw(ctl.address, 'i', [cb.checked ? 1 : 0]);
    });
    value.subscribe((v) => {
      if (v !== null && cb.checked !== v) cb.checked = v;
    });

    dispatcher.registerCodecListener(ctl.address, (msg: OscMessage) => {
      if (msg.types === 'i' && typeof msg.args[0] === 'number') {
        value.set(msg.args[0] !== 0);
      }
    });

    wrap.appendChild(cb);
  } else if (ctl.kind === 'range') {
    const value = new Signal<number | null>(null);

    const slider = document.createElement('input');
    slider.type = 'range';
    slider.min = String(ctl.min);
    slider.max = String(ctl.max);
    slider.step = String(ctl.step);
    slider.value = '0';

    const readout = document.createElement('span');
    readout.className = 'range-readout';
    readout.textContent = `0 ${ctl.unit}`;

    // Continuous sliders (most range controls) throttle outbound writes to
    // avoid saturating the serial bridge. Discrete steppers (step >= 1)
    // send unthrottled since each click is intentional.
    const useThrottle = ctl.step < 1;
    slider.addEventListener('input', () => {
      const v = parseFloat(slider.value);
      value.set(v);
      if (useThrottle) dispatcher.sendRawThrottled(ctl.address, 'f', [v]);
      else             dispatcher.sendRaw(ctl.address, 'f', [v]);
    });
    value.subscribe((v) => {
      if (v !== null) {
        readout.textContent = `${v.toFixed(ctl.step >= 1 ? 0 : 1)} ${ctl.unit}`;
        if (parseFloat(slider.value) !== v) slider.value = String(v);
      }
    });

    dispatcher.registerCodecListener(ctl.address, (msg: OscMessage) => {
      if (msg.types === 'f' && typeof msg.args[0] === 'number') {
        value.set(msg.args[0]);
      } else if (msg.types === 'i' && typeof msg.args[0] === 'number') {
        // Some "i" leaves (like /dac/biquads) drive a range slider.
        value.set(msg.args[0]);
      }
    });

    wrap.appendChild(slider);
    wrap.appendChild(readout);
  } else if (ctl.kind === 'action') {
    const btn = document.createElement('button');
    btn.textContent = 'Send';
    btn.addEventListener('click', () => {
      dispatcher.sendRaw(ctl.address, '', []);
    });
    wrap.appendChild(btn);
  } else if (ctl.kind === 'client_toggle') {
    // Client-only toggle. signalKey selects which in-browser signal it
    // drives. Currently 'eq_link' is the only one; add cases as needed.
    const signal = ctl.signalKey === 'eq_link' ? eqLinkSignal : null;
    if (!signal) return wrap;

    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.addEventListener('change', () => {
      signal.set(cb.checked);
    });
    signal.subscribe((v) => {
      if (cb.checked !== v) cb.checked = v;
      const target = ctl.disableSection;
      if (target) {
        const sec = sectionElements.get(target);
        if (sec) sec.classList.toggle('section-disabled', v === true);
      }
    });

    wrap.appendChild(cb);
  }

  return wrap;
}
