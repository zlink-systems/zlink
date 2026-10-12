import { AsyncLocalStorage } from 'node:async_hooks';
import type { ZLinkRequest } from '../../contracts/Handlers/Attributes';

// This context carries an observation callback, never physical admission state.
const requestAdmission = new AsyncLocalStorage<(() => void) | undefined>();

export function captureRequestAdmission(): (() => void) | undefined {
  return requestAdmission.getStore();
}

export function bindRequestAdmission<T>(operation: () => T): () => T {
  const admitted = captureRequestAdmission();
  if (admitted === undefined) return operation;
  return () => requestAdmission.run(admitted, operation);
}

export function createZLinkRequest<TReply>(
  start: () => Promise<TReply>,
  deliverReply?: (reply: Promise<TReply>) => Promise<TReply>
): ZLinkRequest<TReply> {
  let resolveAdmission!: () => void;
  let rejectAdmission!: (error: unknown) => void;
  const admitted = new Promise<void>((resolve, reject) => {
    resolveAdmission = resolve;
    rejectAdmission = reject;
  });
  // Existing callers may observe only the reply Promise.
  void admitted.catch(() => undefined);
  let pending: Promise<TReply>;
  try {
    pending = requestAdmission.run(resolveAdmission, start);
    const forwarded = (pending as Partial<ZLinkRequest<TReply>>).admitted;
    if (forwarded !== undefined) void forwarded.then(resolveAdmission, () => undefined);
  } catch (error) {
    pending = Promise.reject(error);
  }
  const observed = pending.then(
    (value) => {
      // A successful reply proves admission even without an observation callback.
      resolveAdmission();
      return value;
    },
    (error: unknown) => {
      rejectAdmission(error);
      throw error;
    }
  );
  const reply = deliverReply?.(observed) ?? observed;
  return Object.defineProperty(reply, 'admitted', { value: admitted }) as ZLinkRequest<TReply>;
}
