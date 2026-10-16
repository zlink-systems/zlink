import type { ZLinkRequest } from '../../contracts/Handlers/Attributes';

const completedAdmission = Promise.resolve();
const ignoreAdmissionFailure = (): void => {};

class RequestAdmission {
  private admitted: Promise<void> | undefined;
  private resolveAdmission: (() => void) | undefined;
  private rejectAdmission: ((error: unknown) => void) | undefined;
  confirm(): void {
    this.admitted ??= completedAdmission;
    this.resolveAdmission?.();
  }
  fail(error: unknown): void {
    if (this.admitted === undefined) {
      this.admitted = Promise.reject(error);
      // Existing callers may observe only the reply Promise. Observing the
      // rejection here leaves the public admission Promise rejected.
      void this.admitted.catch(ignoreAdmissionFailure);
    } else {
      this.rejectAdmission?.(error);
    }
  }
  observe(): Promise<void> {
    if (this.admitted === undefined) {
      this.admitted = new Promise<void>((resolve, reject) => {
        this.resolveAdmission = resolve;
        this.rejectAdmission = reject;
      });
      void this.admitted.catch(ignoreAdmissionFailure);
    }
    return this.admitted;
  }
}

export function createZLinkRequest<TReply>(
  start: (onAdmitted: () => void) => Promise<TReply>,
  deliverReply?: (reply: Promise<TReply>) => Promise<TReply>
): ZLinkRequest<TReply> {
  const admission = new RequestAdmission();
  let pending: Promise<TReply>;
  try {
    pending = start(() => admission.confirm());
  } catch (error) {
    pending = Promise.reject(error);
  }
  const observed = pending.then(
    (value) => {
      // A successful reply proves admission even without an observation callback.
      admission.confirm();
      return value;
    },
    (error: unknown) => {
      admission.fail(error);
      throw error;
    }
  );
  const reply = deliverReply?.(observed) ?? observed;
  return Object.defineProperty(reply, 'admitted', {
    get: () => admission.observe()
  }) as ZLinkRequest<TReply>;
}
