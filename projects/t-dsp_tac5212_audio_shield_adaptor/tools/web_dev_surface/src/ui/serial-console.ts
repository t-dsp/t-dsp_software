// Scrollback console for the plain-text side of the multiplexed serial stream
// (boot banners, Serial.println debug output) plus our own outbound/inbound
// OSC echo logging.
//
// Performance evolution:
//
// - v1: textContent = lines.join('\n') on every append. ~15k string
//   ops/sec under meter traffic. Killed it.
// - v2 (commit d0c3ba90): one DOM div per line, ring-buffer trim at
//   MAX_LINES. Constant-time per append.
// - v3 (this version): RAF-batched. append() pushes the line into a
//   pending queue and schedules a single rAF flush. Per frame we
//   build a DocumentFragment, append all queued lines at once, trim
//   to MAX_LINES, and write scrollTop ONCE. Worst case: one forced
//   layout per frame (~60/sec) regardless of input rate.
//
// At sustained high log rates this drops the main-thread cost from
// O(N reflows) to O(1 reflow) per frame.
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

  // Pending lines awaiting the next rAF flush. append() just pushes;
  // the flush builds a DocumentFragment, appends in one shot, trims,
  // and reads scrollTop once.
  const pending: string[] = [];
  let flushScheduled = false;

  function flush(): void {
    flushScheduled = false;
    if (pending.length === 0) return;

    // Build all queued line divs into a fragment first so the DOM
    // gets ONE appendChild call regardless of how many lines arrived.
    const frag = document.createDocumentFragment();
    for (const line of pending) {
      const row = document.createElement('div');
      row.className = 'log-line';
      row.textContent = line;
      frag.appendChild(row);
    }
    pending.length = 0;
    view.appendChild(frag);

    // Trim oldest lines past the cap. Done after the append so the
    // total node count is always correct.
    while (view.childNodes.length > MAX_LINES) {
      view.removeChild(view.firstChild!);
    }

    // Single scrollTop write per frame. Reading scrollHeight here
    // forces one layout — but only one, no matter how many lines
    // were appended this frame.
    if (stickToBottom) {
      view.scrollTop = view.scrollHeight;
    }
  }

  function append(line: string): void {
    pending.push(line);
    // Cap the pending queue too, so a sustained flood doesn't grow
    // unbounded between frames. If we somehow get more than MAX_LINES
    // queued in a single frame, drop the oldest — they'd be trimmed
    // in flush() anyway.
    if (pending.length > MAX_LINES) {
      pending.splice(0, pending.length - MAX_LINES);
    }
    if (!flushScheduled) {
      flushScheduled = true;
      requestAnimationFrame(flush);
    }
  }

  function clear(): void {
    // Drop any pending lines too.
    pending.length = 0;
    while (view.firstChild) view.removeChild(view.firstChild);
    stickToBottom = true;
  }

  clearBtn.addEventListener('click', clear);

  return { element: wrap, append, clear };
}
