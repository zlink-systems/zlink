import { detachedMeshDispatchScope } from '../execution';
import {
  currentFlowContext,
  runWithFlow,
  type ZLinkFlowContextValue
} from '../diagnostics/flow-context';
import type { ZLinkRequest } from '../../contracts/Handlers/Attributes';

const completedAdmission = Promise.resolve();
const ignoreAdmissionFailure = (): void => {};
const REQUEST_ENTRY = Symbol('zlink.requestEntry');
const ADMITTED_PROPERTY: PropertyDescriptor = {
  get(this: { readonly [REQUEST_ENTRY]: ZLinkRequestResult<unknown> }): Promise<void> {
    return this[REQUEST_ENTRY].observeAdmission();
  }
};
export const identityRequestReply = <TReply>(value: TReply): TReply => value;

/** Stable request owner; dispatcher indexes never own the public result's lifetime. */
export class ZLinkRequestResult<TReply> {
  private reply?: Promise<TReply>;
  private resolve?: (value: TReply) => void;
  private reject?: (error: unknown) => void;
  private decode: (value: any) => TReply = identityRequestReply;
  private mapFailure: (error: unknown) => unknown = identityRequestReply;
  private flow?: ZLinkFlowContextValue;
  private admitted?: Promise<void>;
  declare private resolveAdmission?: () => void;
  declare private rejectAdmission?: (error: unknown) => void;
  declare deadlineMs?: number;
  declare capacityStop?: AbortController;

  readonly confirm = (): void => {
    this.admitted ??= completedAdmission;
    this.resolveAdmission?.();
  };

  get isPending(): boolean {
    return this.resolve !== undefined;
  }

  get pending(): Promise<TReply> {
    return (this.reply ??= new Promise<TReply>((resolve, reject) => {
      this.resolve = resolve;
      this.reject = reject;
    }));
  }

  prepare<TSource>(
    decode: (value: TSource) => TReply,
    mapFailure: (error: unknown) => unknown
  ): this {
    this.decode = decode;
    this.mapFailure = mapFailure;
    this.flow = currentFlowContext();
    return this;
  }

  /** Adapts a source owned by another operation without adding another terminal registry. */
  observe(source: Promise<unknown>): this {
    this.reply = source.then(
      (value) => {
        try {
          return this.deliver(value);
        } finally {
          this.clearDelivery();
        }
      },
      (error) => {
        try {
          return this.deliverFailure(error);
        } finally {
          this.clearDelivery();
        }
      }
    );
    return this;
  }

  complete(value: unknown): void {
    const resolve = this.resolve!;
    const reject = this.reject!;
    this.resolve = undefined;
    this.reject = undefined;
    this.capacityStop?.abort();
    this.capacityStop = undefined;
    detachedMeshDispatchScope(queueMicrotask, () => {
      try {
        resolve(this.deliver(value));
      } catch (error) {
        reject(error);
      } finally {
        this.clearDelivery();
      }
    });
  }

  fail(error: unknown): void {
    const reject = this.reject!;
    this.resolve = undefined;
    this.reject = undefined;
    this.capacityStop?.abort();
    this.capacityStop = undefined;
    detachedMeshDispatchScope(queueMicrotask, () => {
      try {
        this.deliverFailure(error);
      } catch (failure) {
        reject(failure);
      } finally {
        this.clearDelivery();
      }
    });
  }

  private deliver(value: unknown): TReply {
    try {
      const reply =
        this.flow === undefined
          ? this.decode(value)
          : runWithFlow(this.flow, () => this.decode(value));
      this.confirm();
      return reply;
    } catch (error) {
      this.failAdmission(error);
      throw error;
    }
  }

  private deliverFailure(error: unknown): never {
    let failure: unknown;
    try {
      failure =
        this.flow === undefined
          ? this.mapFailure(error)
          : runWithFlow(this.flow, () => this.mapFailure(error));
    } catch (mapped) {
      failure = mapped;
    }
    this.failAdmission(failure);
    throw failure;
  }

  private clearDelivery(): void {
    this.decode = identityRequestReply;
    this.mapFailure = identityRequestReply;
    this.flow = undefined;
  }

  private failAdmission(error: unknown): void {
    if (this.admitted === undefined) {
      this.admitted = Promise.reject(error);
      void this.admitted.catch(ignoreAdmissionFailure);
    } else this.rejectAdmission?.(error);
  }

  observeAdmission(): Promise<void> {
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

export function prepareZLinkRequestReply<TSource, TReply>(
  entry: ZLinkRequestResult<TSource>,
  decode: (value: TSource) => TReply,
  mapFailure: (error: unknown) => unknown = identityRequestReply
): ZLinkRequestResult<TReply> {
  return (entry as unknown as ZLinkRequestResult<TReply>).prepare(decode, mapFailure);
}

export function observeZLinkRequestReply<TSource, TReply>(
  source: Promise<TSource>,
  decode: (value: TSource) => TReply,
  mapFailure: (error: unknown) => unknown,
  entry: ZLinkRequestResult<TReply>
): ZLinkRequestResult<TReply> {
  return entry.prepare(decode, mapFailure).observe(source);
}

export function rejectZLinkRequestReply<TReply>(
  error: unknown,
  entry: ZLinkRequestResult<TReply>
): ZLinkRequestResult<TReply> {
  void entry.pending;
  entry.fail(error);
  return entry;
}

export function mapZLinkRequestReply<TReply>(result: ZLinkRequestResult<TReply>): Promise<TReply> {
  return result.pending;
}

export function createZLinkRequest<TReply>(
  start: (entry: ZLinkRequestResult<TReply>) => ZLinkRequestResult<TReply>,
  deliverReply?: (reply: Promise<TReply>) => Promise<TReply>
): ZLinkRequest<TReply> {
  const entry = new ZLinkRequestResult<TReply>();
  try {
    start(entry);
  } catch (error) {
    rejectZLinkRequestReply(error, entry);
  }
  const reply = deliverReply?.(entry.pending) ?? entry.pending;
  Object.defineProperty(reply, REQUEST_ENTRY, { value: entry });
  return Object.defineProperty(reply, 'admitted', ADMITTED_PROPERTY) as ZLinkRequest<TReply>;
}
