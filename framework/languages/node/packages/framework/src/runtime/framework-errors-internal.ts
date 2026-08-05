import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../contracts/Errors/ZLinkFrameworkException';

/** Detailed failure reasons owned by Framework runtime bounded contexts. */
export enum ZLinkFrameworkInternalErrorKind {
  ActorRouteNotFound = 'actorRouteNotFound',
  ActorCreateFailed = 'actorCreateFailed',
  ActorAlreadyExists = 'actorAlreadyExists',
  ActorTypeMismatch = 'actorTypeMismatch',
  SpotCreateFailed = 'spotCreateFailed',
  SpotRouteNotFound = 'spotRouteNotFound',
  SpotTypeMismatch = 'spotTypeMismatch',
  ActorSessionNotBound = 'actorSessionNotBound',
  HandlerNotFound = 'handlerNotFound',
  RouteHandlerNotFound = 'routeHandlerNotFound',
  ActorDispatchHandlerNotFound = 'actorDispatchHandlerNotFound',
  PayloadDecodeFailed = 'payloadDecodeFailed',
  RouteNotConnected = 'routeNotConnected',
  RequestTargetNotFound = 'requestTargetNotFound',
  RequestRejected = 'requestRejected',
  RequestProtocolError = 'requestProtocolError',
  RequestFailed = 'requestFailed',
  WorkerQueueFull = 'workerQueueFull',
  WorkerTimedOut = 'workerTimedOut',
  WorkerFailed = 'workerFailed',
  ActorLocationStale = 'actorLocationStale',
  ActorCreateRejected = 'actorCreateRejected',
  ObjectClientNotConfigured = 'objectClientNotConfigured',
  MeshSelectionRequired = 'meshSelectionRequired',
  MeshNotFound = 'meshNotFound',
  InvalidConfiguration = 'invalidConfiguration',
  AlreadySubmitted = 'alreadySubmitted',
  ActorGenerationStale = 'actorGenerationStale',
  ActorMoving = 'actorMoving',
  DeadlineExceeded = 'deadlineExceeded',
  PlacementCapacityExhausted = 'placementCapacityExhausted',
  RoutingIdConflict = 'routingIdConflict',
  SpotGenerationStale = 'spotGenerationStale',
  SpotMoving = 'spotMoving',
  RelocationDataLost = 'relocationDataLost',
  SpotIdConflict = 'spotIdConflict',
  RuntimeShutdown = 'runtimeShutdown',
  RelocationDisabled = 'relocationDisabled',
  RelocationTargetUnavailable = 'relocationTargetUnavailable',
  RelocationFailed = 'relocationFailed',
  InvalidOperation = 'invalidOperation'
}

const INTERNAL_TO_PUBLIC: Readonly<Record<ZLinkFrameworkInternalErrorKind, ZLinkFrameworkErrorKind>> = Object.freeze({
  [ZLinkFrameworkInternalErrorKind.ActorRouteNotFound]: ZLinkFrameworkErrorKind.NotFound,
  [ZLinkFrameworkInternalErrorKind.ActorCreateFailed]: ZLinkFrameworkErrorKind.InternalFailure,
  [ZLinkFrameworkInternalErrorKind.ActorAlreadyExists]: ZLinkFrameworkErrorKind.AlreadyExists,
  [ZLinkFrameworkInternalErrorKind.ActorTypeMismatch]: ZLinkFrameworkErrorKind.TypeMismatch,
  [ZLinkFrameworkInternalErrorKind.SpotCreateFailed]: ZLinkFrameworkErrorKind.InternalFailure,
  [ZLinkFrameworkInternalErrorKind.SpotRouteNotFound]: ZLinkFrameworkErrorKind.NotFound,
  [ZLinkFrameworkInternalErrorKind.SpotTypeMismatch]: ZLinkFrameworkErrorKind.TypeMismatch,
  [ZLinkFrameworkInternalErrorKind.ActorSessionNotBound]: ZLinkFrameworkErrorKind.InvalidOperation,
  [ZLinkFrameworkInternalErrorKind.HandlerNotFound]: ZLinkFrameworkErrorKind.NotFound,
  [ZLinkFrameworkInternalErrorKind.RouteHandlerNotFound]: ZLinkFrameworkErrorKind.NotFound,
  [ZLinkFrameworkInternalErrorKind.ActorDispatchHandlerNotFound]: ZLinkFrameworkErrorKind.NotFound,
  [ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed]: ZLinkFrameworkErrorKind.ProtocolError,
  [ZLinkFrameworkInternalErrorKind.RouteNotConnected]: ZLinkFrameworkErrorKind.Unavailable,
  [ZLinkFrameworkInternalErrorKind.RequestTargetNotFound]: ZLinkFrameworkErrorKind.NotFound,
  [ZLinkFrameworkInternalErrorKind.RequestRejected]: ZLinkFrameworkErrorKind.Rejected,
  [ZLinkFrameworkInternalErrorKind.RequestProtocolError]: ZLinkFrameworkErrorKind.ProtocolError,
  [ZLinkFrameworkInternalErrorKind.RequestFailed]: ZLinkFrameworkErrorKind.InternalFailure,
  [ZLinkFrameworkInternalErrorKind.WorkerQueueFull]: ZLinkFrameworkErrorKind.CapacityExceeded,
  [ZLinkFrameworkInternalErrorKind.WorkerTimedOut]: ZLinkFrameworkErrorKind.DeadlineExceeded,
  [ZLinkFrameworkInternalErrorKind.WorkerFailed]: ZLinkFrameworkErrorKind.InternalFailure,
  [ZLinkFrameworkInternalErrorKind.ActorLocationStale]: ZLinkFrameworkErrorKind.Unavailable,
  [ZLinkFrameworkInternalErrorKind.ActorCreateRejected]: ZLinkFrameworkErrorKind.Rejected,
  [ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured]: ZLinkFrameworkErrorKind.NotConfigured,
  [ZLinkFrameworkInternalErrorKind.MeshSelectionRequired]: ZLinkFrameworkErrorKind.NotConfigured,
  [ZLinkFrameworkInternalErrorKind.MeshNotFound]: ZLinkFrameworkErrorKind.NotFound,
  [ZLinkFrameworkInternalErrorKind.InvalidConfiguration]: ZLinkFrameworkErrorKind.NotConfigured,
  [ZLinkFrameworkInternalErrorKind.AlreadySubmitted]: ZLinkFrameworkErrorKind.InvalidOperation,
  [ZLinkFrameworkInternalErrorKind.ActorGenerationStale]: ZLinkFrameworkErrorKind.InvalidOperation,
  [ZLinkFrameworkInternalErrorKind.ActorMoving]: ZLinkFrameworkErrorKind.Unavailable,
  [ZLinkFrameworkInternalErrorKind.DeadlineExceeded]: ZLinkFrameworkErrorKind.DeadlineExceeded,
  [ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted]: ZLinkFrameworkErrorKind.CapacityExceeded,
  [ZLinkFrameworkInternalErrorKind.RoutingIdConflict]: ZLinkFrameworkErrorKind.AlreadyExists,
  [ZLinkFrameworkInternalErrorKind.SpotGenerationStale]: ZLinkFrameworkErrorKind.InvalidOperation,
  [ZLinkFrameworkInternalErrorKind.SpotMoving]: ZLinkFrameworkErrorKind.Unavailable,
  [ZLinkFrameworkInternalErrorKind.RelocationDataLost]: ZLinkFrameworkErrorKind.DataLost,
  [ZLinkFrameworkInternalErrorKind.SpotIdConflict]: ZLinkFrameworkErrorKind.AlreadyExists,
  [ZLinkFrameworkInternalErrorKind.RuntimeShutdown]: ZLinkFrameworkErrorKind.ShuttingDown,
  [ZLinkFrameworkInternalErrorKind.RelocationDisabled]: ZLinkFrameworkErrorKind.Rejected,
  [ZLinkFrameworkInternalErrorKind.RelocationTargetUnavailable]: ZLinkFrameworkErrorKind.Unavailable,
  [ZLinkFrameworkInternalErrorKind.RelocationFailed]: ZLinkFrameworkErrorKind.InternalFailure,
  [ZLinkFrameworkInternalErrorKind.InvalidOperation]: ZLinkFrameworkErrorKind.InvalidOperation
});

export const ZLINK_FRAMEWORK_INTERNAL_ERROR_KIND_VALUES: Readonly<Record<ZLinkFrameworkInternalErrorKind, number>> = Object.freeze({
  [ZLinkFrameworkInternalErrorKind.ActorRouteNotFound]: 0,
  [ZLinkFrameworkInternalErrorKind.ActorCreateFailed]: 1,
  [ZLinkFrameworkInternalErrorKind.ActorAlreadyExists]: 2,
  [ZLinkFrameworkInternalErrorKind.ActorTypeMismatch]: 3,
  [ZLinkFrameworkInternalErrorKind.SpotCreateFailed]: 4,
  [ZLinkFrameworkInternalErrorKind.SpotRouteNotFound]: 5,
  [ZLinkFrameworkInternalErrorKind.SpotTypeMismatch]: 6,
  [ZLinkFrameworkInternalErrorKind.ActorSessionNotBound]: 7,
  [ZLinkFrameworkInternalErrorKind.HandlerNotFound]: 8,
  [ZLinkFrameworkInternalErrorKind.RouteHandlerNotFound]: 9,
  [ZLinkFrameworkInternalErrorKind.ActorDispatchHandlerNotFound]: 10,
  [ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed]: 11,
  [ZLinkFrameworkInternalErrorKind.RouteNotConnected]: 12,
  [ZLinkFrameworkInternalErrorKind.RequestTargetNotFound]: 13,
  [ZLinkFrameworkInternalErrorKind.RequestRejected]: 14,
  [ZLinkFrameworkInternalErrorKind.RequestProtocolError]: 15,
  [ZLinkFrameworkInternalErrorKind.RequestFailed]: 16,
  [ZLinkFrameworkInternalErrorKind.WorkerQueueFull]: 17,
  [ZLinkFrameworkInternalErrorKind.WorkerTimedOut]: 18,
  [ZLinkFrameworkInternalErrorKind.WorkerFailed]: 19,
  [ZLinkFrameworkInternalErrorKind.ActorLocationStale]: 20,
  [ZLinkFrameworkInternalErrorKind.ActorCreateRejected]: 21,
  [ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured]: 22,
  [ZLinkFrameworkInternalErrorKind.MeshSelectionRequired]: 23,
  [ZLinkFrameworkInternalErrorKind.MeshNotFound]: 24,
  [ZLinkFrameworkInternalErrorKind.InvalidConfiguration]: 25,
  [ZLinkFrameworkInternalErrorKind.AlreadySubmitted]: 26,
  [ZLinkFrameworkInternalErrorKind.ActorGenerationStale]: 27,
  [ZLinkFrameworkInternalErrorKind.ActorMoving]: 28,
  [ZLinkFrameworkInternalErrorKind.DeadlineExceeded]: 29,
  [ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted]: 30,
  [ZLinkFrameworkInternalErrorKind.RoutingIdConflict]: 31,
  [ZLinkFrameworkInternalErrorKind.SpotGenerationStale]: 32,
  [ZLinkFrameworkInternalErrorKind.SpotMoving]: 33,
  [ZLinkFrameworkInternalErrorKind.RelocationDataLost]: 34,
  [ZLinkFrameworkInternalErrorKind.SpotIdConflict]: 35,
  [ZLinkFrameworkInternalErrorKind.RuntimeShutdown]: 36,
  [ZLinkFrameworkInternalErrorKind.RelocationDisabled]: 37,
  [ZLinkFrameworkInternalErrorKind.RelocationTargetUnavailable]: 38,
  [ZLinkFrameworkInternalErrorKind.RelocationFailed]: 39,
  [ZLinkFrameworkInternalErrorKind.InvalidOperation]: 40
});

const INTERNAL_KIND_BY_WIRE_FAILURE_CODE: ReadonlyMap<number, ZLinkFrameworkInternalErrorKind> = new Map(
  Object.entries(ZLINK_FRAMEWORK_INTERNAL_ERROR_KIND_VALUES)
    .map(([kind, code]) => [code + 1, kind as ZLinkFrameworkInternalErrorKind])
);

const INTERNAL_KIND = new WeakMap<ZLinkFrameworkException, ZLinkFrameworkInternalErrorKind>();

export function createInternalFrameworkException(
  kind: ZLinkFrameworkInternalErrorKind,
  message: string,
  causeOrRetry?: unknown,
  cause?: unknown
): ZLinkFrameworkException {
  const error = new ZLinkFrameworkException(
    INTERNAL_TO_PUBLIC[kind],
    message,
    typeof causeOrRetry === 'boolean' ? cause : causeOrRetry
  );
  INTERNAL_KIND.set(error, kind);
  return error;
}

export function internalFrameworkErrorKind(
  error: ZLinkFrameworkException
): ZLinkFrameworkInternalErrorKind | undefined {
  return INTERNAL_KIND.get(error);
}

export function internalFrameworkErrorCode(error: ZLinkFrameworkException): number {
  const kind = INTERNAL_KIND.get(error);
  return kind === undefined ? error.kind : ZLINK_FRAMEWORK_INTERNAL_ERROR_KIND_VALUES[kind];
}

/** Decodes the stateful reply failureCode convention without exposing wire offsets to callers. */
export function internalFrameworkErrorKindFromWireFailureCode(
  failureCode: number
): ZLinkFrameworkInternalErrorKind | undefined {
  return INTERNAL_KIND_BY_WIRE_FAILURE_CODE.get(failureCode);
}

/** Restores a typed failure only when its wire terminal result is canonical. */
export function internalFrameworkErrorKindFromWireReply(
  terminalResult: number,
  failureCode: number
): ZLinkFrameworkInternalErrorKind | undefined {
  // ServiceStaleGenerationError is emitted as NotFound with the historical
  // stale-route code, while the same internal code is also used by Conflict
  // replies from the actor route protocol.
  if (terminalResult === 102 && failureCode === 21) {
    return ZLinkFrameworkInternalErrorKind.ActorLocationStale;
  }
  const kind = internalFrameworkErrorKindFromWireFailureCode(failureCode);
  if (kind === undefined) return undefined;
  const expectedTerminalResult = [1, 6, 8, 9, 10, 11, 14].includes(failureCode)
    ? 102
    : [2, 5, 13, 17, 19, 20, 35].includes(failureCode)
      ? 105
      : [3, 4, 7, 21, 33, 34].includes(failureCode)
        ? 107
        : [12, 16].includes(failureCode)
          ? 104
          : [15, 18, 22].includes(failureCode)
            ? 106
            : undefined;
  return expectedTerminalResult === terminalResult ? kind : undefined;
}

const BOUNDARY_WIRE_TERMINAL_RESULTS = new Set([
  101, 103, 108, 109, 110, 111, 112, 113
]);

/** Checks the terminal and failure-code pair before a transport maps it. */
export function isCanonicalWireReplyTerminal(
  terminalResult: number,
  failureCode: number
): boolean {
  if (terminalResult === 0) return failureCode === 0;
  if (BOUNDARY_WIRE_TERMINAL_RESULTS.has(terminalResult)) return failureCode === 0;
  return internalFrameworkErrorKindFromWireReply(terminalResult, failureCode) !== undefined;
}
