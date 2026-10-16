/** The typed cancellation error produced by Framework admission helpers. */
export class ZLinkAbortError extends Error {
  constructor(message = 'The operation was aborted.') {
    super(message);
    this.name = 'AbortError';
  }
}

export class ZLinkDeadlineExceededError extends Error {
  constructor(message = 'The operation deadline was exceeded.') {
    super(message);
    this.name = 'DeadlineExceededError';
  }
}

export function createAbortError(): ZLinkAbortError {
  return new ZLinkAbortError();
}

export function isAbortError(error: unknown): boolean {
  return (
    error instanceof ZLinkAbortError ||
    (error instanceof DOMException && error.name === 'AbortError')
  );
}

export function createDeadlineExceededError(message?: string): ZLinkDeadlineExceededError {
  return new ZLinkDeadlineExceededError(message);
}

export function isDeadlineExceededError(error: unknown): boolean {
  return error instanceof ZLinkDeadlineExceededError;
}

export function throwIfAborted(signal: AbortSignal | undefined): void {
  if (signal?.aborted === true) {
    throw createAbortError();
  }
}

/**
 * Whether a handler or dispatch result is a thenable the caller must wait
 * for. Synchronous results, including null and undefined, are not.
 */
export function isPromiseLike<T>(value: T | PromiseLike<T>): value is PromiseLike<T> {
  return (
    value !== null &&
    (typeof value === 'object' || typeof value === 'function') &&
    typeof (value as { then?: unknown }).then === 'function'
  );
}

export function macrotaskBoundary(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}

export function awaitWithAbort<T>(
  operation: Promise<T>,
  signal: AbortSignal | undefined,
  abortOperation?: () => void
): Promise<T> {
  if (signal === undefined) {
    return operation;
  }
  return new Promise<T>((resolve, reject) => {
    let settled = false;
    const settle = (): boolean => {
      if (settled) return false;
      settled = true;
      signal.removeEventListener('abort', abort);
      return true;
    };
    const abort = () => {
      if (!settle()) return;
      abortOperation?.();
      reject(createAbortError());
    };
    operation.then(
      (value) => {
        if (settle()) resolve(value);
      },
      (error) => {
        if (settle()) reject(error);
      }
    );
    if (signal.aborted) abort();
    else signal.addEventListener('abort', abort, { once: true });
  });
}
