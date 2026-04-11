// Scrollback console for the plain-text side of the multiplexed serial stream
// (boot banners, Serial.println debug output) plus our own outbound/inbound
// OSC echo logging.
//
// Performance note: the earlier implementation rebuilt the entire text via
// `view.textContent = lines.join('\n')` on every append. At 30 Hz meter
// traffic that's 15k+ string ops/sec plus a forced layout reflow on every
// append, which pegged the main thread under screen recording. The current
// implementation appends one DOM node per line and removes the oldest when
// exceeding MAX_LINES — constant-time per append, no string concatenation,
// no full-pane re-render.
//
// See main.ts LOG_BLOCKED_ADDRESSES for which OSC addresses are filtered
// OUT of the log to keep the pane readable.

const MAX_LINES = 500;

export interface SerialConsole {
  element: HTMLElement;
  append: (line: string) => void;
  clear: () => void;
}

export function serialConsole(): SerialConsole {
  const wrap = document.createElement('div');
  wrap.className = 'console-wrap';

  const headerRow = document.createElement('div');
  headerRow.className = 'console-header';

  const label = document.createElement('h4');
  label.textContent = 'Serial console';

  const clearBtn = document.createElement('button');
  clearBtn.textContent = 'Clear';
  clearBtn.className = 'console-clear';

  headerRow.append(label, clearBtn);

  const view = document.createElement('div');
  view.className = 'console';

  wrap.append(headerRow, view);

  // Ring buffer of line DIVs. New lines are appendChild'd; when the
  // count exceeds MAX_LINES we removeChild the first one. This keeps
  // per-append work O(1) regardless of scrollback size.
  //
  // We use a single <div> per line rather than a single <pre> + text
  // node so `view.textContent` isn't re-allocated on each append.
  // The CSS for `.console .log-line` should set white-space: pre
  // and a monospace font.

  // Track whether the user has scrolled up; if they have, don't auto-
  // scroll on new appends so they can read without being yanked to the
  // bottom.
  let stickToBottom = true;
  view.addEventListener('scroll', () => {
    const nearBottom =
      view.scrollHeight - view.scrollTop - view.clientHeight < 8;
    stickToBottom = nearBottom;
  });

  function append(line: string): void {
    const row = document.createElement('div');
    row.className = 'log-line';
    row.textContent = line;
    view.appendChild(row);

    // Trim from the front if over the limit. childNodes indexing is
    // cheap on a small list; at MAX_LINES=500 this is always just
    // removing one node.
    while (view.childNodes.length > MAX_LINES) {
      view.removeChild(view.firstChild!);
    }

    if (stickToBottom) {
      // Use scrollTop on the container; the last child being in-flow
      // means scrollHeight is already up-to-date after appendChild.
      view.scrollTop = view.scrollHeight;
    }
  }

  function clear(): void {
    // Remove all children. Fast for up to ~500 nodes.
    while (view.firstChild) view.removeChild(view.firstChild);
    stickToBottom = true;
  }

  clearBtn.addEventListener('click', clear);

  return { element: wrap, append, clear };
}
