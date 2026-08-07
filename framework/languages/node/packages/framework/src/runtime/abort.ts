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

export class ZLinkDeferredCompletion<T> {
  private settled = false;
  private readonly resolvePromise: (value: T) => void;
  private readonly rejectPromise: (error: unknown) => void;
  readonly promise: Promise<T>;

  constructor() {
    let resolvePromise!: (value: T) => void;
    let rejectPromise!: (error: unknown) => void;
    this.promise = new Promise<T>((resolve, reject) => {
      resolvePromise = resolve;
      rejectPromise = reject;
    });
    this.resolvePromise = resolvePromise;
    this.rejectPromise = rejectPromise;
  }

  resolve(value: T): boolean {
    if (!this.claim()) return false;
    this.resolvePromise(value);
    return true;
  }

  reject(error: unknown): boolean {
    if (!this.claim()) return false;
    this.rejectPromise(error);
    return true;
  }

  wait(signal?: AbortSignal): Promise<T> {
    return awaitWithAbort(
      this.promise,
      signal,
      () => this.reject(createAbortError())
    );
  }

  private claim(): boolean {
    if (this.settled) return false;
    this.settled = true;
    return true;
  }
}

export function awaitWithAbort<T>(
  operation: Promise<T>,
  signal: AbortSignal | undefined,
  abortOperation?: () => void
): Promise<T> {
  if (signal?.aborted === true) {
    abortOperation?.();
    throw createAbortError();
  }
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
    signal.addEventListener('abort', abort, { once: true });
    operation.then(
      (value) => { if (settle()) resolve(value); },
      (error) => { if (settle()) reject(error); }
    );
  });
}
