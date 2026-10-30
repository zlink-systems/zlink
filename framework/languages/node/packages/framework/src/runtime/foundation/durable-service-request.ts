import { ConnectionAttemptState, type ConnectionAttempt } from '@zlink-systems/zlink';
import type { OperationHandle, PendingOperation } from './operation-registry';
import { remainingRequestTimeout } from '../messaging/request-deadline';
import { RequestResult, SubmitResult, isZLinkBackendResultError } from '../backend/runtime-values';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';

export interface PeerRouteObservation {
  readonly generation?: bigint;
  readonly ready: boolean;
  readonly selected?: boolean;
  readonly lifecycleGeneration?: bigint;
  readonly endpoint?: string;
  /** Immutable socket snapshot; admission selects this target endpoint only while waiting. */
  readonly connectionAttempts?: readonly ConnectionAttempt[];
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

/** Submit §5: one admission decision for application and lifecycle requests. */
export function admitServiceRequest(
  port: Pick<
    DurableServiceRequestPort,
    | 'requestStopSignal'
    | 'observePeerRoute'
    | 'observePeerRouteChanged'
    | 'observePeerConnectionIntentRemoved'
  >,
  target: string,
  pending: OperationHandle<unknown>,
  start: (remainingMs: number, finished: () => void) => void,
  failed: (error: unknown) => void,
  operationKind = 'Request'
): void {
  let baseline: PeerRouteObservation | undefined;
  let submitted = false;
  let disposed = false;
  let unobserveRoute = () => {};
  let unobserveIntent = () => {};
  const cleanup = () => {
    if (disposed) return;
    disposed = true;
    unobserveRoute();
    unobserveIntent();
    port.requestStopSignal?.removeEventListener('abort', stopped);
    if (pending.promise === undefined) pending.clearFailureDispatcher(cleanup);
  };
  const fail = (error: unknown) => {
    if (disposed) return;
    cleanup();
    failed(error);
  };
  const lifecycleFailure = (error: unknown) => {
    if (!submitted) fail(error);
    else {
      cleanup();
      pending.fail(error);
    }
  };
  const unavailable = (message: string) =>
    lifecycleFailure(
      createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RouteNotConnected,
        message,
        true
      )
    );
  const stopped = () => {
    const error = port.requestStopSignal?.reason;
    lifecycleFailure(error);
    pending.fail(error);
  };
  const check = (): boolean => {
    if (disposed || submitted) return true;
    if (port.requestStopSignal?.aborted === true) {
      stopped();
      return true;
    }
    if (!pending.isPending()) {
      cleanup();
      return true;
    }
    try {
      const remaining = remainingRequestTimeout(operationKind, pending.deadlineMs)!;
      const observation = port.observePeerRoute(target);
      const attempts =
        baseline !== undefined || !observation.ready
          ? observation.connectionAttempts?.filter(
              (attempt) => attempt.endpoint === observation.endpoint
            )
          : undefined;
      if (
        baseline !== undefined &&
        ((baseline.lifecycleGeneration !== undefined &&
          baseline.lifecycleGeneration !== observation.lifecycleGeneration) ||
          (baseline.endpoint !== undefined && baseline.endpoint !== observation.endpoint))
      ) {
        unavailable('Request target descriptor changed.');
        return true;
      }
      // Compare the waiting request's tokens before accepting a recovered route.
      if (
        baseline !== undefined &&
        (baseline.connectionAttempts ?? []).some(
          (previous) =>
            attempts?.some(
              (current) =>
                current.intentId === previous.intentId &&
                current.lastFailureId === previous.lastFailureId
            ) !== true
        )
      ) {
        unavailable('Request connection intent ended, changed, or failed.');
        return true;
      }
      if (
        baseline !== undefined &&
        (baseline.connectionAttempts ?? []).length === 0 &&
        attempts?.some((attempt) => attempt.lastFailureId !== 0n) === true
      ) {
        unavailable('Request first connection observation contains a failure.');
        return true;
      }
      if (observation.ready) {
        submitted = true;
        unobserveRoute();
        start(remaining, cleanup);
        return true;
      }
      if (
        observation.selected !== true &&
        attempts?.some((attempt) => attempt.state === ConnectionAttemptState.AttemptFailed) === true
      ) {
        unavailable('Request connection attempt failed.');
        return true;
      }
      if (baseline === undefined || (baseline.connectionAttempts ?? []).length === 0)
        baseline = { ...observation, connectionAttempts: attempts };
    } catch (error) {
      fail(error);
      return true;
    }
    return false;
  };
  if (check()) return;

  unobserveRoute = port.observePeerRouteChanged((rid) => {
    if (rid === target) check();
  });
  unobserveIntent = port.observePeerConnectionIntentRemoved((rid) => {
    if (rid === target) unavailable('Request target node lifecycle ended.');
  });
  if (pending.promise === undefined) pending.setFailureDispatcher(cleanup);
  else
    void pending.promise.then(cleanup, (error) => {
      if (!submitted) fail(error);
      else cleanup();
    });
  port.requestStopSignal?.addEventListener('abort', stopped, { once: true });
  if (port.requestStopSignal?.aborted === true) stopped();
  else check();
}

/** The lifecycle adapter preserves its physical reply decoder and sender operation. */
export function requestDurableService(
  port: DurableServiceRequestPort,
  target: string,
  parts: readonly Uint8Array[],
  pending: PendingOperation<unknown>,
  operationKind: string,
  signal?: AbortSignal,
  onSubmitted?: (signal: AbortSignal) => Promise<void>,
  onAdmitted?: () => void
): Promise<readonly Buffer[]> {
  const stop = new AbortController();
  const cancellation = AbortSignal.any([
    stop.signal,
    ...(signal === undefined ? [] : [signal]),
    ...(port.requestStopSignal === undefined ? [] : [port.requestStopSignal])
  ]);
  const cancelled = () => pending.fail(cancellation.reason);
  cancellation.addEventListener('abort', cancelled, { once: true });
  const complete = () => {
    cancellation.removeEventListener('abort', cancelled);
    stop.abort();
  };
  void pending.promise.then(complete, complete);
  return new Promise((resolve, reject) => {
    if (cancellation.aborted) {
      cancelled();
      reject(cancellation.reason);
      return;
    }
    admitServiceRequest(
      port,
      target,
      pending,
      (remaining, finished) => {
        const reply = port.requestService(
          target,
          parts,
          remaining,
          onAdmitted,
          pending,
          undefined,
          onSubmitted === undefined
            ? undefined
            : () => {
                try {
                  void onSubmitted(cancellation).catch((error) => pending.fail(error));
                } catch (error) {
                  pending.fail(error);
                }
              }
        );
        void reply.then(
          (value) => {
            finished();
            resolve(value);
          },
          (error) => {
            finished();
            reject(error);
          }
        );
      },
      (error) => {
        pending.fail(error);
        reject(error);
      },
      operationKind
    );
  });
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
