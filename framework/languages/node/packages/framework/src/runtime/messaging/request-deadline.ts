import { isZLinkBackendResultError, RequestResult } from '../backend/runtime-values';
import { createAbortError, isAbortError } from '../abort';
import { OperationTimeoutError, type PendingOperation } from '../foundation/operation-registry';
import {
  createInternalFrameworkException,
  ZLinkFrameworkInternalErrorKind
} from '../framework-errors-internal';

export function remainingRequestTimeout(
  operation: string,
  deadlineMs: number | undefined
): number | undefined {
  if (deadlineMs === undefined) return undefined;
  const remaining = deadlineMs - performance.now();
  if (remaining <= 0) throw requestTimeout(operation);
  return Math.max(1, Math.ceil(remaining));
}

export function requestTerminalError(operation: string, error: unknown): unknown {
  return error instanceof OperationTimeoutError ||
    (isZLinkBackendResultError(error) &&
      error.operation === 'request' &&
      error.result === RequestResult.TimedOut)
    ? requestTimeout(operation)
    : error;
}

function requestTimeout(operation: string): Error {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
    `${operation} exceeded its deadline.`
  );
}

/** Observes a stage without registering or completing the logical request. */
export function waitRequestReply<T>(
  stage: Promise<T>,
  operation: string,
  pending: PendingOperation<unknown>,
  endWait?: () => void,
  discardLateReply?: (reply: T) => void
): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const fail = (error: unknown): void => {
      endWait?.();
      if (discardLateReply !== undefined) void stage.then(discardLateReply, () => undefined);
      reject(isAbortError(error) ? createAbortError() : requestTerminalError(operation, error));
    };
    pending.setFailureDispatcher(fail);
    void stage.then(
      (value) => {
        pending.clearFailureDispatcher(fail);
        resolve(value);
      },
      (error) => {
        const terminal = requestTerminalError(operation, error);
        if (terminal !== error) {
          if (!pending.fail(terminal)) void pending.promise.then(() => fail(terminal), fail);
        } else {
          pending.clearFailureDispatcher(fail);
          fail(error);
        }
      }
    );
  });
}
