import { Tab, Control } from '../codec-panel-config';
import { Dispatcher } from '../dispatcher';

export function codecPanel(tabs: Tab[], dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'codec-panel';

  const tabBar = document.createElement('div');
  tabBar.className = 'tab-bar';

  const content = document.createElement('div');
  content.className = 'tab-content';

  let activeIdx = 0;
  const tabButtons: HTMLButtonElement[] = [];

  tabs.forEach((tab, i) => {
    const btn = document.createElement('button');
    btn.className = 'tab-btn';
    btn.textContent = tab.name;
    btn.addEventListener('click', () => {
      activeIdx = i;
      render();
    });
    tabBar.appendChild(btn);
    tabButtons.push(btn);
  });

  function render(): void {
    tabButtons.forEach((b, i) => b.classList.toggle('active', i === activeIdx));
    content.innerHTML = '';
    const tab = tabs[activeIdx];
    for (const group of tab.groups) {
      const g = document.createElement('div');
      g.className = 'group';
      const h = document.createElement('h4');
      h.textContent = group.name;
      g.appendChild(h);
      for (const ctl of group.controls) {
        g.appendChild(renderControl(ctl, dispatcher));
      }
      content.appendChild(g);
    }
  }

  render();
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
    const sel = document.createElement('select');
    for (const opt of ctl.options) {
      const o = document.createElement('option');
      o.value = opt;
      o.textContent = opt;
      sel.appendChild(o);
    }
    sel.addEventListener('change', () => {
      dispatcher.sendRaw(ctl.address, 's', [sel.value]);
    });
    wrap.appendChild(sel);
  } else if (ctl.kind === 'toggle') {
    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.addEventListener('change', () => {
      dispatcher.sendRaw(ctl.address, 'i', [cb.checked ? 1 : 0]);
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
