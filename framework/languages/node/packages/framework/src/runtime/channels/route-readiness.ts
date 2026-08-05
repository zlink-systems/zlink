import { createAbortError, throwIfAborted } from '../abort';

export function isTransientRouteNotReadyError(error: unknown): boolean {
  const message = error instanceof Error ? error.message : String(error);
  return message.includes('Host unreachable') ||
    message.includes('not ready') ||
    message.includes('async submit timed out');
}

export function delay(milliseconds: number, signal: AbortSignal | undefined): Promise<void> {
  throwIfAborted(signal);
  return new Promise((resolve, reject) => {
    let abort: (() => void) | undefined;
    const timeout = setTimeout(() => {
      if (abort !== undefined) {
        signal?.removeEventListener('abort', abort);
      }
      resolve();
    }, milliseconds);
    if (signal === undefined) {
      return;
    }
    abort = () => {
      clearTimeout(timeout);
      reject(createAbortError());
    };
    signal.addEventListener('abort', abort, { once: true });
  });
}
