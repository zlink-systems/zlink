import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  requestResultToPublicErrorKind
} from '../framework-errors-internal';
import { ZLinkFrameworkException } from '../../contracts';
import { RequestResult, SubmitResult } from '../backend/runtime-values';
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

/** Projects the Core submit result once; phase only determines capacity refusal meaning. */
export function submitToRequestResult(result: number, phase: 'submit' | 'completion'): number {
  switch (result) {
    case SubmitResult.Ok:
      return RequestResult.Ok;
    case SubmitResult.Backpressured:
      return phase === 'submit' ? RequestResult.NotConnected : RequestResult.TimedOut;
    case SubmitResult.NotConnected:
      return RequestResult.NotConnected;
    case SubmitResult.NotFound:
      return RequestResult.NotFound;
    case SubmitResult.NotAdmitted:
      return RequestResult.Rejected;
    case SubmitResult.InvalidHandle:
    case SubmitResult.InvalidArgument:
    case SubmitResult.ThreadViolation:
      return RequestResult.InvalidArgument;
    case SubmitResult.InvalidState:
      return RequestResult.InvalidState;
    case SubmitResult.NotSupported:
      return RequestResult.NotSupported;
    case SubmitResult.Terminated:
      return RequestResult.Terminated;
    default:
      return RequestResult.InternalError;
  }
}

export function classifySubmitResult(
  result: number,
  operation: string,
  phase: 'submit' | 'completion' = 'completion'
): ZLinkSubmitResult {
  const terminal = submitToRequestResult(result, phase);
  switch (terminal) {
    case RequestResult.Ok:
      return { status: ZLinkSubmitStatus.Submitted };
    case RequestResult.TimedOut:
      return { status: ZLinkSubmitStatus.TimedOut };
    case RequestResult.NotFound:
      return { status: ZLinkSubmitStatus.TargetNotFound };
    case RequestResult.NotConnected:
      return { status: ZLinkSubmitStatus.RouteNotConnected };
    case RequestResult.Terminated:
      return { status: ZLinkSubmitStatus.Shutdown };
    default:
      throw new ZLinkFrameworkException(
        requestResultToPublicErrorKind(terminal),
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
