// Codec panel — declarative descriptor (codec-panel-config.ts) -> reactive UI.
//
// Each enum/toggle control has a Signal<...> for its current chip-side value.
// On construction we register one dispatcher listener per control so that
// /codec/<model>/.../leaf echoes from the firmware (notably the /snapshot
// reply on connect) update the signal, which in turn updates the DOM.
//
// Tab switching toggles CSS display rather than destroying and re-creating
// nodes — that keeps the dispatcher listeners stable and avoids re-binding
// every time the user clicks a tab. Cheap and the panel is small.

import { Tab, Control } from '../codec-panel-config';
import { Dispatcher } from '../dispatcher';
import { Signal } from '../state';
import { OscMessage } from '../osc';

export function codecPanel(tabs: Tab[], dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'codec-panel';

  const tabBar = document.createElement('div');
  tabBar.className = 'tab-bar';

  const content = document.createElement('div');
  content.className = 'tab-content';

  const tabPanels: HTMLElement[] = [];
  const tabButtons: HTMLButtonElement[] = [];

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
    for (const group of tab.groups) {
      const g = document.createElement('div');
      g.className = 'group';
      const h = document.createElement('h4');
      h.textContent = group.name;
      g.appendChild(h);
      for (const ctl of group.controls) {
        g.appendChild(renderControl(ctl, dispatcher));
      }
      panel.appendChild(g);
    }
    content.appendChild(panel);
    tabPanels.push(panel);
  });

  selectTab(0);
  root.append(tabBar, content);
  return root;
}

function renderControl(ctl: Control, dispatcher: Dispatcher): HTMLElement {
  const wrap = document.createElement('div');
  wrap.className = 'control';

  const label = document.createElement('label');
  label.textContent = ctl.label;
  wrap.appendChild(label);

  if (ctl.kind === 'enum') {
    // Signal holds the most recent value the firmware reported (or null
    // if we haven't seen one yet — the <select> falls back to its first
    // option in that case, but the snapshot reply on connect should
    // populate it before the user sees the panel).
    const value = new Signal<string | null>(null);

    const sel = document.createElement('select');
    for (const opt of ctl.options) {
      const o = document.createElement('option');
      o.value = opt;
      o.textContent = opt;
      sel.appendChild(o);
    }
    sel.addEventListener('change', () => {
      // Optimistic update so the UI doesn't flicker between the user's
      // pick and the firmware echo. Echo will land here too and re-set
      // the same value (idempotent).
      value.set(sel.value);
      dispatcher.sendRaw(ctl.address, 's', [sel.value]);
    });
    value.subscribe((v) => {
      if (v !== null && sel.value !== v) sel.value = v;
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
  } else {
    const btn = document.createElement('button');
    btn.textContent = 'Send';
    btn.addEventListener('click', () => {
      dispatcher.sendRaw(ctl.address, '', []);
    });
    wrap.appendChild(btn);
  }

  return wrap;
}
