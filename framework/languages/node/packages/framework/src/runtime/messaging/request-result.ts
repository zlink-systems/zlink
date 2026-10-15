import type { ZLinkRequest } from '../../contracts/Handlers/Attributes';

const completedAdmission = Promise.resolve();
const ignoreAdmissionFailure = (): void => {};

export function createZLinkRequest<TReply>(
  start: (onAdmitted: () => void) => Promise<TReply>,
  deliverReply?: (reply: Promise<TReply>) => Promise<TReply>
): ZLinkRequest<TReply> {
  let admitted: Promise<void> | undefined;
  let resolveAdmission: (() => void) | undefined;
  let rejectAdmission: ((error: unknown) => void) | undefined;
  const confirmAdmission = (): void => {
    admitted ??= completedAdmission;
    resolveAdmission?.();
  };
  const failAdmission = (error: unknown): void => {
    if (admitted === undefined) {
      admitted = Promise.reject(error);
      // Existing callers may observe only the reply Promise. Observing the
      // rejection here leaves the public admission Promise rejected.
      void admitted.catch(ignoreAdmissionFailure);
    } else {
      rejectAdmission?.(error);
    }
  };
  const observeAdmission = (): Promise<void> => {
    if (admitted === undefined) {
      admitted = new Promise<void>((resolve, reject) => {
        resolveAdmission = resolve;
        rejectAdmission = reject;
      });
      void admitted.catch(ignoreAdmissionFailure);
    }
    return admitted;
  };
  let pending: Promise<TReply>;
  try {
    pending = start(confirmAdmission);
  } catch (error) {
    pending = Promise.reject(error);
  }
  const observed = pending.then(
    (value) => {
      // A successful reply proves admission even without an observation callback.
      confirmAdmission();
      return value;
    },
    (error: unknown) => {
      failAdmission(error);
      throw error;
    }
  );
  const reply = deliverReply?.(observed) ?? observed;
  return Object.defineProperty(reply, 'admitted', {
    get: observeAdmission
  }) as ZLinkRequest<TReply>;
}
