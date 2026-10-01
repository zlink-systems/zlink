import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import { SubmitResult } from '../backend/runtime-values';
export enum ZLinkSubmitStatus {
  Submitted = 'submitted',
  Backpressured = 'backpressured',
  TimedOut = 'timedOut',
  TargetNotFound = 'targetNotFound',
  RouteNotConnected = 'routeNotConnected',
  Shutdown = 'shutdown'
}

export interface ZLinkSubmitResult {
  readonly status: ZLinkSubmitStatus;
}

/**
 * The single owner that classifies a binding SubmitResult as a Framework submit status.
 * NotConnected is a route that cannot be used (Unavailable); it is never a backpressure signal.
 */
export function classifySubmitResult(result: number, operation: string): ZLinkSubmitResult {
  switch (result) {
    case SubmitResult.Ok:
      return { status: ZLinkSubmitStatus.Submitted };
    case SubmitResult.Backpressured:
      return { status: ZLinkSubmitStatus.Backpressured };
    case SubmitResult.NotAdmitted:
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestRejected,
        `${operation} was not admitted.`
      );
    case SubmitResult.NotFound:
      return { status: ZLinkSubmitStatus.TargetNotFound };
    case SubmitResult.NotConnected:
      return { status: ZLinkSubmitStatus.RouteNotConnected };
    case SubmitResult.Terminated:
      return { status: ZLinkSubmitStatus.Shutdown };
    case SubmitResult.InvalidState:
    case SubmitResult.InvalidArgument:
    case SubmitResult.InvalidHandle:
    case SubmitResult.ThreadViolation:
      // Spec 07-framework-error-model §2: a submit in the wrong state or with a bad handle or
      // argument is an invalid operation, not a missing target.
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        `${operation} failed with submit result ${result}.`
      );
    default:
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestFailed,
        `${operation} failed with submit result ${result}.`
      );
  }
}

export function requireOneWayCompletion(
  result: ZLinkSubmitResult,
  operation: string,
  notFoundKind: ZLinkFrameworkInternalErrorKind = ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
): void {
  switch (result.status) {
    case ZLinkSubmitStatus.Submitted:
      return;
    case ZLinkSubmitStatus.TimedOut:
    case ZLinkSubmitStatus.Backpressured:
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
        `${operation} did not obtain admission before its deadline.`,
        true
      );
    case ZLinkSubmitStatus.TargetNotFound:
      throw createInternalFrameworkException(notFoundKind, `${operation} target was not found.`);
    case ZLinkSubmitStatus.RouteNotConnected:
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RouteNotConnected,
        `${operation} route is not connected.`,
        true
      );
    case ZLinkSubmitStatus.Shutdown:
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
        `${operation} was rejected because the runtime is shutting down.`
      );
  }
}

export function requirePublishCompletion(result: ZLinkSubmitResult, operation: string): void {
  requireOneWayCompletion(result, operation);
}

export function throwAlreadySubmitted(operation: string): never {
  throw createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.AlreadySubmitted,
    `${operation} has already been submitted.`
  );
}
