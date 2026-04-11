// Scrollback console for the plain-text side of the multiplexed serial stream
// (boot banners, Serial.println debug output) plus our own outbound/inbound
// OSC echo logging.

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

  const lines: string[] = [];

  function render(): void {
    view.textContent = lines.join('\n');
    view.scrollTop = view.scrollHeight;
  }

  function append(line: string): void {
    lines.push(line);
    if (lines.length > MAX_LINES) {
      lines.splice(0, lines.length - MAX_LINES);
    }
    render();
  }

  function clear(): void {
    lines.length = 0;
    render();
  }

  clearBtn.addEventListener('click', clear);

  return { element: wrap, append, clear };
}
