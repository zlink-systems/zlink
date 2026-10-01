import {
  Disposable,
  ZlinkStreamError,
  ZlinkStreamErrorCode,
  ZlinkStreamException
} from '../Contracts';

const BACKING_ARRAY_COMPACTION_MIN_HEAD = 1024;

/** Returns whether the consumed prefix should compact the backing array. */
export function shouldCompactBackingArray(head: number, length: number): boolean {
  return head >= BACKING_ARRAY_COMPACTION_MIN_HEAD && head * 2 >= length;
}

export function connectorError(
  code: ZlinkStreamErrorCode,
  message: string,
  cause?: unknown
): ZlinkStreamException {
  return new ZlinkStreamException({ code, message, cause });
}

export function toStreamError(
  cause: unknown,
  code: ZlinkStreamErrorCode,
  message: string
): ZlinkStreamError {
  if (cause instanceof ZlinkStreamException) {
    return cause.error;
  }
  return { code, message, cause };
}

export function unwrapStreamError(error: unknown): ZlinkStreamError {
  if (error instanceof ZlinkStreamException) {
    return error.error;
  }
  return {
    code: ZlinkStreamErrorCode.RemoteError,
    message: error instanceof Error ? error.message : String(error),
    cause: error
  };
}

export function subscription(dispose: () => void): Disposable {
  return { dispose };
}

/** Snapshot order is preserved; membership owns each registration's lifetime. */
export function* currentRegistrations<T>(
  registrations: ReadonlySet<T>,
  snapshot: readonly T[] = Array.from(registrations)
): IterableIterator<T> {
  for (const registration of snapshot) {
    if (registrations.has(registration)) yield registration;
  }
}

export interface HandlerRegistration<T> {
  readonly handler: T;
}

export function registerHandler<T>(handlers: Set<HandlerRegistration<T>>, handler: T): Disposable {
  const registration = { handler };
  handlers.add(registration);
  return subscription(() => handlers.delete(registration));
}

export function throwIfAborted(signal: AbortSignal | undefined): void {
  if (signal?.aborted === true) {
    throw connectorError(ZlinkStreamErrorCode.Disconnected, 'Operation canceled.');
  }
}

export function delay(delayMs: number, signal: AbortSignal | undefined): Promise<void> {
  throwIfAborted(signal);
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      signal?.removeEventListener('abort', onAbort);
      resolve();
    }, delayMs);
    const onAbort = () => {
      clearTimeout(timeout);
      reject(connectorError(ZlinkStreamErrorCode.Disconnected, 'Operation canceled.'));
    };
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

export function utf8Encode(value: string): Uint8Array {
  return new TextEncoder().encode(value);
}

export function utf8Decode(value: Uint8Array): string {
  return new TextDecoder().decode(value);
}
