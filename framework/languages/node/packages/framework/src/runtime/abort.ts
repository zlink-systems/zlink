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
  return error instanceof ZLinkAbortError
    || (error instanceof DOMException && error.name === 'AbortError');
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

export function awaitWithAbort<T>(operation: Promise<T>, signal: AbortSignal | undefined): Promise<T> {
  throwIfAborted(signal);
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
      if (settle()) reject(createAbortError());
    };
    signal.addEventListener('abort', abort, { once: true });
    operation.then(
      (value) => { if (settle()) resolve(value); },
      (error) => { if (settle()) reject(error); }
    );
  });
}
