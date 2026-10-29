import type { PendingOperation } from './operation-registry';
import { awaitWithAbort } from '../abort';
import { RequestResult, SubmitResult, isZLinkBackendResultError } from '../backend/runtime-values';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';

export interface PeerRouteObservation {
  readonly generation?: bigint;
  readonly ready: boolean;
}

export interface DurableServiceRequestPort {
  readonly requestStopSignal?: AbortSignal;
  observePeerRoute(target: string): PeerRouteObservation;
  observePeerRouteChanged(listener: (target: string) => void): () => void;
  observePeerConnectionIntentRemoved(listener: (target: string) => void): () => void;
  requestService(
    target: string,
    parts: readonly Uint8Array[],
    timeoutMs: number,
    onAdmitted?: () => void,
    existing?: Pick<PendingOperation<unknown>, 'id' | 'deadlineMs'>,
    receiver?: undefined,
    onSubmitted?: () => void
  ): Promise<readonly Buffer[]>;
}

/** Initial readiness belongs to Framework; a started request belongs to the binding. */
export async function requestDurableService(
  port: DurableServiceRequestPort,
  target: string,
  parts: readonly Uint8Array[],
  pending: PendingOperation<unknown>,
  operationKind: string,
  signal?: AbortSignal,
  onSubmitted?: (signal: AbortSignal) => Promise<void>
): Promise<readonly Buffer[]> {
  const stop = new AbortController();
  const unobserveIntent = port.observePeerConnectionIntentRemoved((rid) => {
    if (rid === target)
      pending.fail(
        createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RouteNotConnected,
          `${operationKind} target node lifecycle ended.`,
          true
        )
      );
  });
  void pending.promise.then(
    () => {
      unobserveIntent();
      stop.abort();
    },
    () => {
      unobserveIntent();
      stop.abort();
    }
  );
  const cancellation = AbortSignal.any([
    stop.signal,
    ...(signal === undefined ? [] : [signal]),
    ...(port.requestStopSignal === undefined ? [] : [port.requestStopSignal])
  ]);
  const cancelled = () => pending.fail(cancellation.reason);
  cancellation.addEventListener('abort', cancelled, { once: true });
  try {
    if (cancellation.aborted) cancelled();
    while (pending.isPending()) {
      const remainingMs = Math.ceil(pending.deadlineMs! - performance.now());
      if (remainingMs <= 0) break;
      let changed!: () => void;
      const transition = new Promise<void>((resolve) => {
        changed = resolve;
      });
      const unobserveRoute = port.observePeerRouteChanged((rid) => {
        if (rid === target && port.observePeerRoute(target).ready) changed();
      });
      try {
        if (!port.observePeerRoute(target).ready) {
          await awaitWithAbort(transition, cancellation);
          continue;
        }
      } finally {
        unobserveRoute();
      }
      if (!pending.isPending()) break;
      const reply = port.requestService(
        target,
        parts,
        remainingMs,
        undefined,
        pending,
        undefined,
        onSubmitted === undefined
          ? undefined
          : () => {
              try {
                void onSubmitted(cancellation).catch((error) => {
                  pending.fail(error);
                });
              } catch (error) {
                pending.fail(error);
              }
            }
      );
      // Public completion stops the hook, while this physical reply still reaches its decoder.
      return await reply;
    }
    return (await pending.promise) as readonly Buffer[];
  } finally {
    cancellation.removeEventListener('abort', cancelled);
  }
}

export function durableRequestCanReplay(error: unknown): boolean {
  if (!isZLinkBackendResultError(error)) return false;
  return error.operation === 'request'
    ? error.result === RequestResult.NotConnected || error.result === RequestResult.TimedOut
    : error.result === SubmitResult.NotConnected ||
        error.result === SubmitResult.NotFound ||
        error.result === SubmitResult.NotAdmitted ||
        error.result === SubmitResult.Backpressured;
}
