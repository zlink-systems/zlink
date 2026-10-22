import { currentFlowContext, runWithFlow } from '../diagnostics/flow-context';
import type { ZLinkRequest } from '../../contracts/Handlers/Attributes';

const completedAdmission = Promise.resolve();
const ignoreAdmissionFailure = (): void => {};
const REQUEST_ADMISSION = Symbol('zlink.requestAdmission');
// Every public request shares this getter. A getter closure per request gives
// each reply Promise its own hidden class, and V8 then keeps its properties in
// a per-request dictionary.
const ADMITTED_PROPERTY: PropertyDescriptor = {
  get(this: { readonly [REQUEST_ADMISSION]: RequestAdmission }): Promise<void> {
    return this[REQUEST_ADMISSION].observe();
  }
};

/** Transport preparation is separate from typed result delivery. */
export interface ZLinkRequestResult<TReply> {
  readonly pending: Promise<unknown>;
  decode(value: unknown): TReply;
  readonly mapFailure: (error: unknown) => unknown;
}

export const identityRequestReply = <TReply>(value: TReply): TReply => value;

export function prepareZLinkRequestReply<TSource, TReply>(
  pending: Promise<TSource>,
  decode: (value: TSource) => TReply,
  mapFailure: (error: unknown) => unknown = identityRequestReply
): ZLinkRequestResult<TReply> {
  const flow = currentFlowContext();
  if (flow === undefined) return { pending, decode, mapFailure };
  // The completion reaction is registered after the outbound flow scope returns.
  return {
    pending,
    decode: (value: TSource) => runWithFlow(flow, () => decode(value)),
    mapFailure: (error: unknown) => runWithFlow(flow, () => mapFailure(error))
  };
}

export function rejectZLinkRequestReply<TReply>(error: unknown): ZLinkRequestResult<TReply> {
  return prepareZLinkRequestReply(Promise.reject<TReply>(error), identityRequestReply);
}

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

function requestFailure<TReply>(result: ZLinkRequestResult<TReply>, error: unknown): unknown {
  try {
    return result.mapFailure(error);
  } catch (mappedError) {
    return mappedError;
  }
}

/** Yield applies the same transport decoding without public admission. */
export function mapZLinkRequestReply<TReply>(result: ZLinkRequestResult<TReply>): Promise<TReply> {
  return result.pending.then(
    (value) => result.decode(value),
    (error: unknown) => {
      throw requestFailure(result, error);
    }
  );
}

export function createZLinkRequest<TReply>(
  start: (onAdmitted: () => void) => ZLinkRequestResult<TReply>,
  deliverReply?: (reply: Promise<TReply>) => Promise<TReply>
): ZLinkRequest<TReply> {
  const admission = new RequestAdmission();
  const confirm = (): void => admission.confirm();
  let pending: Promise<TReply>;
  try {
    const result = start(confirm);
    pending = result.pending.then(
      (value) => {
        try {
          const reply = result.decode(value);
          admission.confirm();
          return reply;
        } catch (error) {
          admission.fail(error);
          throw error;
        }
      },
      (error: unknown) => {
        const failure = requestFailure(result, error);
        admission.fail(failure);
        throw failure;
      }
    );
  } catch (error) {
    admission.fail(error);
    pending = Promise.reject(error);
  }
  const reply = deliverReply?.(pending) ?? pending;
  Object.defineProperty(reply, REQUEST_ADMISSION, { value: admission });
  return Object.defineProperty(reply, 'admitted', ADMITTED_PROPERTY) as ZLinkRequest<TReply>;
}
