import type { ZLinkRequest } from '../../contracts/Handlers/Attributes';

const completedAdmission = Promise.resolve();
const ignoreAdmissionFailure = (): void => {};

class RequestAdmission<TReply> {
  private admitted: Promise<void> | undefined;
  private resolveAdmission: (() => void) | undefined;
  private rejectAdmission: ((error: unknown) => void) | undefined;
  // The admission owner also supplies the descriptor, avoiding a second
  // object that only forwards the public getter to this owner.
  readonly get = this.observe.bind(this);

  confirm(): void;
  confirm(value: TReply): TReply;
  confirm(value?: TReply): TReply | undefined {
    this.admitted ??= completedAdmission;
    this.resolveAdmission?.();
    return value;
  }
  fail(error: unknown): never {
    if (this.admitted === undefined) {
      this.admitted = Promise.reject(error);
      // Existing callers may observe only the reply Promise. Observing the
      // rejection here leaves the public admission Promise rejected.
      void this.admitted.catch(ignoreAdmissionFailure);
    } else {
      this.rejectAdmission?.(error);
    }
    throw error;
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
  const admission = new RequestAdmission<TReply>();
  // Both the owner's notification and a successful reply prove admission.
  const confirm = admission.confirm.bind(admission);
  let pending: Promise<TReply>;
  try {
    pending = start(confirm);
  } catch (error) {
    pending = Promise.reject(error);
  }
  const observed = pending.then(confirm, admission.fail.bind(admission));
  const reply = deliverReply?.(observed) ?? observed;
  return Object.defineProperty(reply, 'admitted', admission) as ZLinkRequest<TReply>;
}
