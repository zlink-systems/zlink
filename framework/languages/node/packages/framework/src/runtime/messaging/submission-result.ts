import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
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
      throw createInternalFrameworkException(
        notFoundKind,
        `${operation} target was not found.`
      );
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

export function requirePublishCompletion(
  result: ZLinkSubmitResult,
  operation: string
): void {
  requireOneWayCompletion(result, operation);
}

export function throwAlreadySubmitted(operation: string): never {
  throw createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.AlreadySubmitted,
    `${operation} has already been submitted.`
  );
}
