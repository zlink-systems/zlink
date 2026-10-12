import type { ZLinkRequest } from '../../contracts/Handlers/Attributes';

export function createZLinkRequest<TReply>(
  start: (onAdmitted: () => void) => Promise<TReply>,
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
    pending = start(resolveAdmission);
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
