# main.ts diff — moving the Codec panel to its own tab

This is a sketch of the changes that need to land in
`tools/web_dev_surface/src/main.ts` to (1) add a new top-level **Codec**
tab and (2) remove the inline codec section from the Mixer tab.

The diff below is illustrative — actual line numbers will shift as the
file evolves.

## 1. Add a Codec view-tab button

```diff
 const mixerTab = document.createElement('button');
 mixerTab.className = 'view-tab active';
 mixerTab.dataset.view = 'mixer';
 mixerTab.textContent = 'Mixer';
+const codecTab = document.createElement('button');
+codecTab.className = 'view-tab';
+codecTab.dataset.view = 'codec';
+codecTab.textContent = 'Codec';
 const spectrumTab = document.createElement('button');
 ...
-viewTabs.append(mixerTab, spectrumTab, synthTab, fxTab, processingTab, loopTab, beatsTab, clockTab, arpTab);
+viewTabs.append(mixerTab, codecTab, spectrumTab, synthTab, fxTab, processingTab, loopTab, beatsTab, clockTab, arpTab);
```

## 2. Move the codec section out of the Mixer view

```diff
 const mixerView = document.createElement('section');
 mixerView.className = 'view view-mixer';

 const mixerRow = document.createElement('section');
 mixerRow.className = 'mixer-row';
 ...

-const codecSection = document.createElement('section');
-codecSection.className = 'codec-section';
-codecSection.appendChild(codecPanel(tac5212Panel, dispatcher));
-
 const rawSection = document.createElement('section');
 rawSection.className = 'raw-section';
 const rawLabel = document.createElement('h4');
 rawLabel.textContent = 'Raw OSC';
 rawSection.append(rawLabel, rawOsc(dispatcher, log));

-mixerView.append(mixerRow, codecSection, rawSection);
+mixerView.append(mixerRow, rawSection);
```

## 3. Add the Codec view section

```diff
+// --- Codec view section -------------------------------------------
+//
+// Hierarchical codec configuration. Sub-tabs cover signal chain,
+// DAC EQ, dynamics, DAC filter & volume, ADC, routing/reference,
+// and system. See codec-panel-config.ts for the descriptor and
+// ui/codec-panel.ts for the renderer.
+const codecView = document.createElement('section');
+codecView.className = 'view view-codec';
+codecView.style.display = 'none';
+codecView.appendChild(codecPanel(tac5212Panel, dispatcher));
```

## 4. Wire the new tab into selectView

```diff
-type ViewName = 'mixer' | 'spectrum' | 'synth' | 'fx' | 'processing' | 'loop' | 'beats' | 'clock' | 'arp';
+type ViewName = 'mixer' | 'codec' | 'spectrum' | 'synth' | 'fx' | 'processing' | 'loop' | 'beats' | 'clock' | 'arp';

 function selectView(name: ViewName): void {
   mixerTab.classList.toggle('active',      name === 'mixer');
+  codecTab.classList.toggle('active',      name === 'codec');
   spectrumTab.classList.toggle('active',   name === 'spectrum');
   ...
   mixerView.style.display         = name === 'mixer'      ? '' : 'none';
+  codecView.style.display         = name === 'codec'      ? '' : 'none';
   spectrumSection.style.display   = name === 'spectrum'   ? '' : 'none';
   ...
 }
 mixerTab.addEventListener('click',      () => selectView('mixer'));
+codecTab.addEventListener('click',      () => selectView('codec'));
 spectrumTab.addEventListener('click',   () => selectView('spectrum'));
 ...

-app.append(header, viewTabs, mixerView, spectrumSection, synthSection, fxSection, processingSection, loopSection, beatsSection, clockSection, arpSection, console_.element);
+app.append(header, viewTabs, mixerView, codecView, spectrumSection, synthSection, fxSection, processingSection, loopSection, beatsSection, clockSection, arpSection, console_.element);
```

## 5. CSS — Codec view layout

`style.css` (or wherever the existing `.codec-section` and
`.view-mixer .mixer-row` rules live) gets a `.view-codec` block that
gives the codec panel breathing room since it's no longer cramped at
the bottom of the mixer tab. Approximate target:

```css
.view-codec {
  max-width: 1100px;
  margin: 1rem auto;
  display: flex;
  flex-direction: column;
  gap: 1rem;
}

/* Section card visuals (consumed by the new section header rules) */
.codec-section-card {
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--surface);
  overflow: hidden;
}
.codec-section-header {
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  font-size: 0.95rem;
  border-bottom: 1px solid var(--border);
  padding: 0.5rem 0.75rem;
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: 0.75rem;
  cursor: pointer;
  user-select: none;
}
.codec-section-header .title {
  display: flex;
  align-items: baseline;
  gap: 0.5rem;
}
.codec-section-header .chevron {
  font-size: 0.75rem;
  color: var(--muted);
}
.codec-section-subtitle {
  font-weight: 400;
  font-size: 0.85rem;
  color: var(--muted);
  text-transform: none;
  letter-spacing: normal;
}
.codec-section-presets {
  display: flex;
  gap: 0.25rem;
}
.codec-section-presets button {
  font-size: 0.75rem;
  padding: 0.15rem 0.5rem;
}
.codec-section-body {
  padding: 0.5rem 0.75rem 0.75rem;
  display: grid;
  grid-template-columns: minmax(140px, 1fr) 2fr;
  row-gap: 0.5rem;
  column-gap: 1rem;
  align-items: center;
}
.codec-section-body[hidden] { display: none; }
.codec-control-help {
  grid-column: 2;
  font-size: 0.78rem;
  color: var(--muted);
  margin-top: -0.25rem;
}
```
