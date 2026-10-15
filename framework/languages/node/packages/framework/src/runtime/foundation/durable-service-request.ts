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
    onAdmitted?: () => void
  ): Promise<readonly Buffer[]>;
}

/** Actor model §8.1: a ready edge or a typed transport failure owns replay. */
export async function requestDurableService(
  port: DurableServiceRequestPort,
  target: string,
  parts: readonly Uint8Array[],
  deadlineMs: number,
  operationKind: string,
  signal?: AbortSignal,
  onSubmitted?: (signal: AbortSignal) => Promise<void>
): Promise<readonly Buffer[]> {
  let wasAdmitted = false;
  const stop = new AbortController();
  const exhausted = (cause?: unknown) =>
    createInternalFrameworkException(
      wasAdmitted
        ? ZLinkFrameworkInternalErrorKind.DeadlineExceeded
        : ZLinkFrameworkInternalErrorKind.RouteNotConnected,
      `${operationKind} exhausted its end-to-end deadline.`,
      true,
      cause
    );
  const timer = setTimeout(
    () => stop.abort(exhausted()),
    Math.max(0, Math.ceil(deadlineMs - performance.now()))
  );
  const unobserveIntent = port.observePeerConnectionIntentRemoved((rid) => {
    if (rid === target)
      stop.abort(
        createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RouteNotConnected,
          `${operationKind} target node lifecycle ended.`,
          true
        )
      );
  });
  const cancellation = AbortSignal.any([
    stop.signal,
    ...(signal === undefined ? [] : [signal]),
    ...(port.requestStopSignal === undefined ? [] : [port.requestStopSignal])
  ]);
  try {
    for (;;) {
      cancellation.throwIfAborted();
      const remainingMs = Math.ceil(deadlineMs - performance.now());
      if (remainingMs <= 0) throw exhausted();
      let changed!: () => void;
      const transition = new Promise<void>((resolve) => {
        changed = resolve;
      });
      let submittedGeneration: bigint | undefined;
      const attemptStop = new AbortController();
      const attemptSignal = AbortSignal.any([cancellation, attemptStop.signal]);
      const unobserveRoute = port.observePeerRouteChanged((rid) => {
        if (rid !== target) return;
        const route = port.observePeerRoute(target);
        if (
          submittedGeneration === undefined
            ? route.ready
            : route.generation !== undefined && route.generation !== submittedGeneration
        ) {
          changed();
          attemptStop.abort();
        }
      });
      try {
        const route = port.observePeerRoute(target);
        if (!route.ready) {
          await awaitWithAbort(transition, cancellation);
          continue;
        }
        submittedGeneration = route.generation;
        const reply = port.requestService(target, parts, remainingMs, () => {
          wasAdmitted = true;
        });
        const request =
          onSubmitted === undefined
            ? reply
            : Promise.race([
                reply,
                Promise.resolve()
                  .then(() => onSubmitted(attemptSignal))
                  .then(() => reply)
              ]);
        const result = await awaitWithAbort(
          Promise.race([request.then((parts) => ({ parts })), transition.then(() => undefined)]),
          cancellation
        );
        if (result !== undefined) return result.parts;
      } catch (error) {
        cancellation.throwIfAborted();
        if (!durableRequestCanReplay(error)) throw error;
        if (isZLinkBackendResultError(error) && error.operation === 'request') wasAdmitted = true;
      } finally {
        attemptStop.abort();
        unobserveRoute();
      }
    }
  } finally {
    clearTimeout(timer);
    unobserveIntent();
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
