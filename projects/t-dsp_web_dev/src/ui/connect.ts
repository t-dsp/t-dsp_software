import { Signal } from '../state';

export function connectButton(
  connected: Signal<boolean>,
  onConnect: () => void | Promise<void>,
  onDisconnect: () => void | Promise<void>,
): HTMLElement {
  const btn = document.createElement('button');
  btn.className = 'connect';

  connected.subscribe((c) => {
    btn.textContent = c ? 'Disconnect' : 'Connect';
    btn.classList.toggle('connected', c);
  });

  btn.addEventListener('click', () => {
    if (connected.get()) onDisconnect();
    else onConnect();
  });

  return btn;
}
