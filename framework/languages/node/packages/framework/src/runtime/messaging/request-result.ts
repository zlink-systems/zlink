import type { ZLinkRequest } from '../../contracts/Handlers/Attributes';

const completedAdmission = Promise.resolve();
const ignoreAdmissionFailure = (): void => {};

export interface ZLinkRequestReplyObserver {
  confirm<TReply>(value: TReply): TReply;
  fail(error: unknown): void;
}

export const identityRequestReply = <TReply>(value: TReply): TReply => value;

/** Stateless identity observer for internal requests without public admission. */
export const passthroughRequestReplyObserver: ZLinkRequestReplyObserver = {
  confirm: identityRequestReply,
  fail: (): void => {}
};

/** Maps a reply and its admission from the same completion turn. */
export function mapZLinkRequestReply<TSource, TReply>(
  pending: Promise<TSource>,
  decode: (value: TSource) => TReply,
  observer: ZLinkRequestReplyObserver = passthroughRequestReplyObserver,
  mapFailure: (error: unknown) => unknown = identityRequestReply
): Promise<TReply> {
  return pending.then(
    (value) => {
      try {
        return observer.confirm(decode(value));
      } catch (error) {
        observer.fail(error);
        throw error;
      }
    },
    (error: unknown) => {
      let failure: unknown;
      try {
        failure = mapFailure(error);
      } catch (mappedError) {
        failure = mappedError;
      }
      observer.fail(failure);
      throw failure;
    }
  );
}

class RequestAdmission implements ZLinkRequestReplyObserver {
  private admitted: Promise<void> | undefined;
  private resolveAdmission: (() => void) | undefined;
  private rejectAdmission: ((error: unknown) => void) | undefined;
  confirm(): void;
  confirm<TReply>(value: TReply): TReply;
  confirm<TReply>(value?: TReply): TReply | undefined {
    this.admitted ??= completedAdmission;
    this.resolveAdmission?.();
    return value;
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
  start: (onAdmitted: () => void, observer: ZLinkRequestReplyObserver) => Promise<TReply>,
  deliverReply?: (reply: Promise<TReply>) => Promise<TReply>
): ZLinkRequest<TReply> {
  const admission = new RequestAdmission();
  // Both the owner's notification and a successful reply prove admission.
  const confirm = admission.confirm.bind(admission);
  let pending: Promise<TReply>;
  try {
    pending = start(confirm, admission);
  } catch (error) {
    admission.fail(error);
    pending = Promise.reject(error);
  }
  const reply = deliverReply?.(pending) ?? pending;
  return Object.defineProperty(reply, 'admitted', {
    get: () => admission.observe()
  }) as ZLinkRequest<TReply>;
}
