import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../contracts/Errors/ZLinkFrameworkException';
import { RequestResult } from './backend/runtime-values';
import type { ServiceWireRelocationObject } from './foundation/service-stateful-wire-codec';
import { ServiceWireProtocolError } from './foundation/service-wire-m6a-codec';
import {
  ServiceWireBoundaryTerminalResults,
  ServiceWireExactTerminalByFailureCode,
  ServiceWireFrameworkErrorCode
} from './foundation/service-wire-constants.generated';

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
  InvalidOperation = 'invalidOperation',
  ActorRouteUnavailable = 'actorRouteUnavailable'
}

type FailureContext = 'reply' | 'actor' | 'relocation';
interface FailureMapping {
  readonly internalKind?: ZLinkFrameworkInternalErrorKind;
  readonly publicKind: ZLinkFrameworkErrorKind;
  readonly internalValue?: number;
  readonly send: number;
  readonly sendTerminal?: number;
  readonly relocationActor?: number;
  readonly relocationSpot?: number;
  readonly replyTerminalAlias?: number;
  readonly actorBindingReply?: { readonly terminalResult: number; readonly failureCode: number };
  readonly receive?: Partial<Record<FailureContext, readonly number[]>>;
}

// This table owns kind/value/wire correspondence, including existing context aliases.
// The generated wire schema continues to own valid terminal/failure combinations.
const FAILURE_MAPPINGS: readonly FailureMapping[] = [
  {
    internalKind: ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
    publicKind: ZLinkFrameworkErrorKind.NotFound,
    internalValue: ServiceWireFrameworkErrorCode.actorRouteNotFound - 1,
    send: ServiceWireFrameworkErrorCode.actorRouteNotFound,
    receive: { relocation: [ServiceWireFrameworkErrorCode.actorRouteNotFound] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
    publicKind: ZLinkFrameworkErrorKind.InternalFailure,
    internalValue: ServiceWireFrameworkErrorCode.actorCreateFailed - 1,
    send: ServiceWireFrameworkErrorCode.actorCreateFailed,
    receive: { relocation: [ServiceWireFrameworkErrorCode.actorCreateFailed] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.ActorAlreadyExists,
    publicKind: ZLinkFrameworkErrorKind.AlreadyExists,
    internalValue: ServiceWireFrameworkErrorCode.actorAlreadyExists - 1,
    send: ServiceWireFrameworkErrorCode.actorAlreadyExists,
    receive: {
      actor: [ServiceWireFrameworkErrorCode.actorAlreadyExists],
      relocation: [ServiceWireFrameworkErrorCode.actorAlreadyExists]
    }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.ActorTypeMismatch,
    publicKind: ZLinkFrameworkErrorKind.TypeMismatch,
    internalValue: ServiceWireFrameworkErrorCode.actorTypeMismatch - 1,
    send: ServiceWireFrameworkErrorCode.actorTypeMismatch,
    receive: {
      actor: [ServiceWireFrameworkErrorCode.actorTypeMismatch],
      relocation: [ServiceWireFrameworkErrorCode.actorTypeMismatch]
    }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.SpotCreateFailed,
    publicKind: ZLinkFrameworkErrorKind.InternalFailure,
    internalValue: ServiceWireFrameworkErrorCode.spotCreateFailed - 1,
    send: ServiceWireFrameworkErrorCode.spotCreateFailed,
    receive: { relocation: [ServiceWireFrameworkErrorCode.spotCreateFailed] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
    publicKind: ZLinkFrameworkErrorKind.NotFound,
    internalValue: ServiceWireFrameworkErrorCode.spotRouteNotFound - 1,
    send: ServiceWireFrameworkErrorCode.spotRouteNotFound,
    receive: { relocation: [ServiceWireFrameworkErrorCode.spotRouteNotFound] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.SpotTypeMismatch,
    publicKind: ZLinkFrameworkErrorKind.TypeMismatch,
    internalValue: ServiceWireFrameworkErrorCode.spotTypeMismatch - 1,
    send: ServiceWireFrameworkErrorCode.spotTypeMismatch,
    receive: {
      actor: [ServiceWireFrameworkErrorCode.spotTypeMismatch],
      relocation: [ServiceWireFrameworkErrorCode.spotTypeMismatch]
    }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
    publicKind: ZLinkFrameworkErrorKind.InvalidOperation,
    internalValue: ServiceWireFrameworkErrorCode.actorSessionNotBound - 1,
    send: ServiceWireFrameworkErrorCode.actorSessionNotBound,
    receive: {
      actor: [ServiceWireFrameworkErrorCode.actorSessionNotBound],
      relocation: [ServiceWireFrameworkErrorCode.actorSessionNotBound]
    }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.HandlerNotFound,
    publicKind: ZLinkFrameworkErrorKind.NotFound,
    internalValue: ServiceWireFrameworkErrorCode.handlerNotFound - 1,
    send: ServiceWireFrameworkErrorCode.handlerNotFound,
    receive: { relocation: [ServiceWireFrameworkErrorCode.handlerNotFound] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.RouteHandlerNotFound,
    publicKind: ZLinkFrameworkErrorKind.NotFound,
    internalValue: ServiceWireFrameworkErrorCode.routeHandlerNotFound - 1,
    send: ServiceWireFrameworkErrorCode.routeHandlerNotFound,
    receive: { relocation: [ServiceWireFrameworkErrorCode.routeHandlerNotFound] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.ActorDispatchHandlerNotFound,
    publicKind: ZLinkFrameworkErrorKind.NotFound,
    internalValue: ServiceWireFrameworkErrorCode.actorDispatchHandlerNotFound - 1,
    send: ServiceWireFrameworkErrorCode.actorDispatchHandlerNotFound,
    receive: { relocation: [ServiceWireFrameworkErrorCode.actorDispatchHandlerNotFound] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed,
    publicKind: ZLinkFrameworkErrorKind.ProtocolError,
    internalValue: ServiceWireFrameworkErrorCode.payloadDecodeFailed - 1,
    send: ServiceWireFrameworkErrorCode.payloadDecodeFailed,
    receive: { relocation: [ServiceWireFrameworkErrorCode.payloadDecodeFailed] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.RouteNotConnected,
    publicKind: ZLinkFrameworkErrorKind.Unavailable,
    internalValue: ServiceWireFrameworkErrorCode.routeNotConnected - 1,
    send: ServiceWireFrameworkErrorCode.routeNotConnected,
    receive: {
      reply: [
        ServiceWireFrameworkErrorCode.routeNotConnected,
        ServiceWireFrameworkErrorCode.workerQueueFull
      ],
      actor: [
        ServiceWireFrameworkErrorCode.routeNotConnected,
        ServiceWireFrameworkErrorCode.workerQueueFull
      ],
      relocation: [ServiceWireFrameworkErrorCode.routeNotConnected]
    }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
    publicKind: ZLinkFrameworkErrorKind.NotFound,
    internalValue: ServiceWireFrameworkErrorCode.requestTargetNotFound - 1,
    send: ServiceWireFrameworkErrorCode.requestTargetNotFound,
    receive: {
      actor: [
        ServiceWireFrameworkErrorCode.handlerNotFound,
        ServiceWireFrameworkErrorCode.requestTargetNotFound
      ],
      relocation: [ServiceWireFrameworkErrorCode.requestTargetNotFound]
    }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.RequestRejected,
    publicKind: ZLinkFrameworkErrorKind.Rejected,
    internalValue: ServiceWireFrameworkErrorCode.requestRejected - 1,
    send: ServiceWireFrameworkErrorCode.requestRejected,
    receive: {
      actor: [ServiceWireFrameworkErrorCode.requestRejected],
      relocation: [ServiceWireFrameworkErrorCode.requestRejected]
    }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.RequestProtocolError,
    publicKind: ZLinkFrameworkErrorKind.ProtocolError,
    internalValue: ServiceWireFrameworkErrorCode.requestProtocolError - 1,
    send: ServiceWireFrameworkErrorCode.requestProtocolError,
    receive: {
      actor: [
        ServiceWireFrameworkErrorCode.payloadDecodeFailed,
        ServiceWireFrameworkErrorCode.requestProtocolError
      ],
      relocation: [ServiceWireFrameworkErrorCode.requestProtocolError]
    }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.RequestFailed,
    publicKind: ZLinkFrameworkErrorKind.InternalFailure,
    internalValue: ServiceWireFrameworkErrorCode.requestFailed - 1,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {
      actor: [ServiceWireFrameworkErrorCode.requestFailed],
      relocation: [ServiceWireFrameworkErrorCode.requestFailed]
    }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.WorkerQueueFull,
    publicKind: ZLinkFrameworkErrorKind.Unavailable,
    internalValue: ServiceWireFrameworkErrorCode.workerQueueFull - 1,
    send: ServiceWireFrameworkErrorCode.workerQueueFull,
    receive: { reply: [], relocation: [ServiceWireFrameworkErrorCode.workerQueueFull] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.WorkerTimedOut,
    publicKind: ZLinkFrameworkErrorKind.DeadlineExceeded,
    internalValue: ServiceWireFrameworkErrorCode.workerTimedOut - 1,
    send: ServiceWireFrameworkErrorCode.workerTimedOut,
    receive: {
      actor: [ServiceWireFrameworkErrorCode.workerTimedOut],
      relocation: [ServiceWireFrameworkErrorCode.workerTimedOut]
    }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.WorkerFailed,
    publicKind: ZLinkFrameworkErrorKind.InternalFailure,
    internalValue: ServiceWireFrameworkErrorCode.workerFailed - 1,
    send: ServiceWireFrameworkErrorCode.workerFailed,
    receive: {
      actor: [ServiceWireFrameworkErrorCode.workerFailed],
      relocation: [ServiceWireFrameworkErrorCode.workerFailed]
    }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.ActorLocationStale,
    replyTerminalAlias: RequestResult.NotFound,
    publicKind: ZLinkFrameworkErrorKind.Unavailable,
    internalValue: ServiceWireFrameworkErrorCode.actorLocationStale - 1,
    send: ServiceWireFrameworkErrorCode.actorLocationStale,
    receive: {
      actor: [ServiceWireFrameworkErrorCode.actorLocationStale],
      relocation: [ServiceWireFrameworkErrorCode.actorLocationStale]
    }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.ActorCreateRejected,
    publicKind: ZLinkFrameworkErrorKind.Rejected,
    internalValue: ServiceWireFrameworkErrorCode.actorCreateRejected - 1,
    send: ServiceWireFrameworkErrorCode.actorCreateRejected,
    receive: { relocation: [ServiceWireFrameworkErrorCode.actorCreateRejected] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured,
    publicKind: ZLinkFrameworkErrorKind.NotConfigured,
    internalValue: 22,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.MeshSelectionRequired,
    publicKind: ZLinkFrameworkErrorKind.NotConfigured,
    internalValue: 23,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.MeshNotFound,
    publicKind: ZLinkFrameworkErrorKind.NotFound,
    internalValue: 24,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.InvalidConfiguration,
    publicKind: ZLinkFrameworkErrorKind.NotConfigured,
    internalValue: 25,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.AlreadySubmitted,
    publicKind: ZLinkFrameworkErrorKind.InvalidOperation,
    internalValue: 26,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.ActorGenerationStale,
    actorBindingReply: {
      terminalResult: RequestResult.InvalidState,
      failureCode: ServiceWireFrameworkErrorCode.none
    },
    publicKind: ZLinkFrameworkErrorKind.InvalidOperation,
    internalValue: 27,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: { actor: [ServiceWireFrameworkErrorCode.spotGenerationStale] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.ActorMoving,
    publicKind: ZLinkFrameworkErrorKind.Unavailable,
    internalValue: 28,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: { actor: [ServiceWireFrameworkErrorCode.spotMoving] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
    publicKind: ZLinkFrameworkErrorKind.DeadlineExceeded,
    internalValue: 29,
    send: ServiceWireFrameworkErrorCode.none,
    sendTerminal: RequestResult.TimedOut,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted,
    publicKind: ZLinkFrameworkErrorKind.Unavailable,
    internalValue: 30,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.RoutingIdConflict,
    publicKind: ZLinkFrameworkErrorKind.AlreadyExists,
    internalValue: 31,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.SpotGenerationStale,
    publicKind: ZLinkFrameworkErrorKind.InvalidOperation,
    internalValue: ServiceWireFrameworkErrorCode.spotGenerationStale - 1,
    send: ServiceWireFrameworkErrorCode.spotGenerationStale,
    receive: { relocation: [ServiceWireFrameworkErrorCode.spotGenerationStale] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.SpotMoving,
    publicKind: ZLinkFrameworkErrorKind.Unavailable,
    internalValue: ServiceWireFrameworkErrorCode.spotMoving - 1,
    send: ServiceWireFrameworkErrorCode.spotMoving,
    receive: { relocation: [ServiceWireFrameworkErrorCode.spotMoving] }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.RelocationDataLost,
    publicKind: ZLinkFrameworkErrorKind.DataLost,
    internalValue: ServiceWireFrameworkErrorCode.relocationDataLost - 1,
    send: ServiceWireFrameworkErrorCode.relocationDataLost,
    receive: {
      actor: [ServiceWireFrameworkErrorCode.relocationDataLost],
      relocation: [ServiceWireFrameworkErrorCode.relocationDataLost]
    }
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.SpotIdConflict,
    publicKind: ZLinkFrameworkErrorKind.AlreadyExists,
    internalValue: 35,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
    publicKind: ZLinkFrameworkErrorKind.ShuttingDown,
    internalValue: 36,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.RelocationDisabled,
    publicKind: ZLinkFrameworkErrorKind.Rejected,
    internalValue: 37,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.RelocationTargetUnavailable,
    publicKind: ZLinkFrameworkErrorKind.Unavailable,
    internalValue: 38,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.RelocationFailed,
    publicKind: ZLinkFrameworkErrorKind.InternalFailure,
    internalValue: 39,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.InvalidOperation,
    publicKind: ZLinkFrameworkErrorKind.InvalidOperation,
    internalValue: 40,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    receive: {}
  },
  {
    internalKind: ZLinkFrameworkInternalErrorKind.ActorRouteUnavailable,
    publicKind: ZLinkFrameworkErrorKind.Unavailable,
    internalValue: ServiceWireFrameworkErrorCode.routeNotConnected - 1,
    send: ServiceWireFrameworkErrorCode.routeNotConnected,
    receive: { reply: [] }
  },
  {
    publicKind: ZLinkFrameworkErrorKind.NotFound,
    send: ServiceWireFrameworkErrorCode.requestTargetNotFound,
    relocationActor: ServiceWireFrameworkErrorCode.requestTargetNotFound,
    relocationSpot: ServiceWireFrameworkErrorCode.requestTargetNotFound
  },
  {
    publicKind: ZLinkFrameworkErrorKind.AlreadyExists,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    relocationActor: ServiceWireFrameworkErrorCode.actorAlreadyExists,
    relocationSpot: ServiceWireFrameworkErrorCode.actorAlreadyExists
  },
  {
    publicKind: ZLinkFrameworkErrorKind.TypeMismatch,
    send: ServiceWireFrameworkErrorCode.actorTypeMismatch,
    relocationActor: ServiceWireFrameworkErrorCode.actorTypeMismatch,
    relocationSpot: ServiceWireFrameworkErrorCode.spotTypeMismatch
  },
  {
    publicKind: ZLinkFrameworkErrorKind.NotConfigured,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    relocationActor: ServiceWireFrameworkErrorCode.handlerNotFound,
    relocationSpot: ServiceWireFrameworkErrorCode.handlerNotFound
  },
  {
    publicKind: ZLinkFrameworkErrorKind.Rejected,
    send: ServiceWireFrameworkErrorCode.requestRejected,
    relocationActor: ServiceWireFrameworkErrorCode.requestRejected,
    relocationSpot: ServiceWireFrameworkErrorCode.requestRejected
  },
  {
    publicKind: ZLinkFrameworkErrorKind.Unavailable,
    send: ServiceWireFrameworkErrorCode.routeNotConnected,
    relocationActor: ServiceWireFrameworkErrorCode.routeNotConnected,
    relocationSpot: ServiceWireFrameworkErrorCode.routeNotConnected
  },
  {
    publicKind: ZLinkFrameworkErrorKind.DeadlineExceeded,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    relocationActor: ServiceWireFrameworkErrorCode.workerTimedOut,
    relocationSpot: ServiceWireFrameworkErrorCode.workerTimedOut
  },
  {
    publicKind: ZLinkFrameworkErrorKind.ShuttingDown,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    relocationActor: ServiceWireFrameworkErrorCode.requestFailed,
    relocationSpot: ServiceWireFrameworkErrorCode.requestFailed
  },
  {
    publicKind: ZLinkFrameworkErrorKind.ProtocolError,
    send: ServiceWireFrameworkErrorCode.requestProtocolError,
    relocationActor: ServiceWireFrameworkErrorCode.requestProtocolError,
    relocationSpot: ServiceWireFrameworkErrorCode.requestProtocolError
  },
  {
    publicKind: ZLinkFrameworkErrorKind.InvalidOperation,
    send: ServiceWireFrameworkErrorCode.actorLocationStale,
    relocationActor: ServiceWireFrameworkErrorCode.actorLocationStale,
    relocationSpot: ServiceWireFrameworkErrorCode.spotGenerationStale
  },
  {
    publicKind: ZLinkFrameworkErrorKind.DataLost,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    relocationActor: ServiceWireFrameworkErrorCode.relocationDataLost,
    relocationSpot: ServiceWireFrameworkErrorCode.relocationDataLost
  },
  {
    publicKind: ZLinkFrameworkErrorKind.InternalFailure,
    send: ServiceWireFrameworkErrorCode.requestFailed,
    relocationActor: ServiceWireFrameworkErrorCode.requestFailed,
    relocationSpot: ServiceWireFrameworkErrorCode.requestFailed
  }
];

const INTERNAL_MAPPING = new Map(
  FAILURE_MAPPINGS.filter((row) => row.internalKind !== undefined).map((row) => [
    row.internalKind!,
    row
  ])
);
const PUBLIC_MAPPING = new Map(
  FAILURE_MAPPINGS.filter((row) => row.internalKind === undefined).map((row) => [
    row.publicKind,
    row
  ])
);
const RECEIVE_MAPPING = new Map<
  FailureContext,
  ReadonlyMap<number, ZLinkFrameworkInternalErrorKind>
>(
  (['reply', 'actor', 'relocation'] as const).map((context) => [
    context,
    new Map(
      FAILURE_MAPPINGS.flatMap((row) =>
        (
          row.receive?.[context] ??
          (context === 'reply' && row.internalValue !== undefined ? [row.internalValue + 1] : [])
        ).map((code) => [code, row.internalKind!] as const)
      )
    )
  ])
);

const REPLY_TERMINAL_ALIASES = new Map(
  FAILURE_MAPPINGS.filter((row) => row.replyTerminalAlias !== undefined).map((row) => [
    row.replyTerminalAlias!,
    new Map([[row.send, row.internalKind!]])
  ])
);

export const ZLINK_FRAMEWORK_INTERNAL_ERROR_KIND_VALUES = Object.freeze(
  Object.fromEntries([...INTERNAL_MAPPING].map(([kind, row]) => [kind, row.internalValue!]))
) as Readonly<Record<ZLinkFrameworkInternalErrorKind, number>>;

export function frameworkRelocationFailureCode(
  kind: ZLinkFrameworkErrorKind,
  objectKind: ServiceWireRelocationObject['kind']
): number {
  const row =
    PUBLIC_MAPPING.get(kind) ?? PUBLIC_MAPPING.get(ZLinkFrameworkErrorKind.InternalFailure)!;
  return objectKind === 'actor' ? row.relocationActor! : row.relocationSpot!;
}

const WIRE_TERMINAL_RESULT_BY_FAILURE_CODE: ReadonlyMap<number, number> = new Map([
  ...Object.entries(ServiceWireExactTerminalByFailureCode).map(
    ([code, terminal]) => [Number(code), terminal] as const
  )
]);

const INTERNAL_KIND = new WeakMap<ZLinkFrameworkException, ZLinkFrameworkInternalErrorKind>();

export function createInternalFrameworkException(
  kind: ZLinkFrameworkInternalErrorKind,
  message: string,
  causeOrRetry?: unknown,
  cause?: unknown
): ZLinkFrameworkException {
  const error = new ZLinkFrameworkException(
    INTERNAL_MAPPING.get(kind)!.publicKind,
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

/**
 * Maps a backend request terminal (RequestResult) to the public framework
 * error kind. Mirrors the authoritative C++ request_failure_mapper /
 * Java ZLinkBackendRequestResult.toFrameworkErrorKind so a client-side request
 * terminal is classified identically across languages instead of collapsing to
 * Unavailable. Spec 32-framework-error-model:81-92 (request-terminal
 * classification); a deadline yields DeadlineExceeded, a lost route Unavailable.
 * Backpressure waits in Core until the operation deadline, so request and
 * one-way terminals both surface DeadlineExceeded when that wait expires.
 */
export function requestResultToPublicErrorKind(result: number): ZLinkFrameworkErrorKind {
  switch (result) {
    case RequestResult.TimedOut:
      return ZLinkFrameworkErrorKind.DeadlineExceeded;
    case RequestResult.NotFound:
      return ZLinkFrameworkErrorKind.NotFound;
    case RequestResult.Terminated:
      return ZLinkFrameworkErrorKind.ShuttingDown;
    case RequestResult.ProtocolError:
      return ZLinkFrameworkErrorKind.ProtocolError;
    case RequestResult.Rejected:
      return ZLinkFrameworkErrorKind.Rejected;
    case RequestResult.Backpressured:
      return ZLinkFrameworkErrorKind.DeadlineExceeded;
    case RequestResult.Conflict:
    case RequestResult.Busy:
    case RequestResult.NotConnected:
      //  Spec 32:99-103 — a remote target's queue/owner state (conflict, busy,
      //  or lost connection) is a resource this runtime does not own: Unavailable.
      return ZLinkFrameworkErrorKind.Unavailable;
    case RequestResult.InvalidArgument:
    case RequestResult.InvalidState:
      return ZLinkFrameworkErrorKind.InvalidOperation;
    case RequestResult.InternalError:
    case RequestResult.NotSupported:
    default:
      return ZLinkFrameworkErrorKind.InternalFailure;
  }
}

/**
 * Ownership-aware classification of a remote reply/completion terminal plus its
 * fine failure code into a public framework exception (spec
 * 32-framework-error-model:81-118, 99-108). A fine failure code refines the
 * coarse terminal; when absent (0) or unrecognised, the coarse terminal
 * classifies. Remote another-node queue/operation-table saturation
 * (Conflict/Busy) is Unavailable — a resource this runtime does not own — while
 * placement admission terminal Backpressured(113) is Unavailable because no
 * node can host the Spot. Shared by the lifecycle
 * completion paths (Actor join, User Spot create/close) so they classify
 * identically to the request path instead of collapsing to a single kind.
 */
export function wireReplyFailureException(
  terminalResult: number,
  failureErrno: number,
  message: string
): ZLinkFrameworkException {
  if (failureErrno !== 0) {
    const fine = internalFrameworkErrorKindFromWireFailureCode(failureErrno);
    if (fine !== undefined) {
      return createInternalFrameworkException(fine, message);
    }
  }
  if (terminalResult === RequestResult.Backpressured) {
    return createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted,
      message
    );
  }
  return new ZLinkFrameworkException(requestResultToPublicErrorKind(terminalResult), message);
}

/**
 * Translates a wire decode/shape failure raised while awaiting a remote
 * lifecycle reply into the public ProtocolError classification.
 * ServiceWireProtocolError is a plain Error thrown by the wire codec when a
 * reply cannot be processed; spec 32-framework-error-model:58-60 and 91-92
 * require the awaiting caller to observe a Framework ProtocolError instead of
 * an untyped transport error. Every other error is returned unchanged so
 * already-classified failures keep their kind.
 */
export function translateWireReplyDecodeError(error: unknown): unknown {
  if (error instanceof ServiceWireProtocolError) {
    return createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RequestProtocolError,
      error.message
    );
  }
  return error;
}

export function internalFrameworkErrorCode(error: ZLinkFrameworkException): number {
  const kind = INTERNAL_KIND.get(error);
  return kind === undefined ? error.kind : ZLINK_FRAMEWORK_INTERNAL_ERROR_KIND_VALUES[kind];
}

/** Produces the canonical stateful wire terminal without leaking internal kinds. */
export function internalFrameworkWireReply(
  error: ZLinkFrameworkException | ZLinkFrameworkInternalErrorKind,
  context: 'canonical' | 'stale-generation' | 'actor-binding' = 'canonical'
): {
  readonly terminalResult: number;
  readonly failureCode: number;
} {
  const kind = typeof error === 'string' ? error : INTERNAL_KIND.get(error);
  const row =
    kind === undefined
      ? (PUBLIC_MAPPING.get((error as ZLinkFrameworkException).kind) ??
        PUBLIC_MAPPING.get(ZLinkFrameworkErrorKind.InternalFailure)!)
      : INTERNAL_MAPPING.get(kind)!;
  const alias = context === 'actor-binding' ? row.actorBindingReply : undefined;
  const failureCode = alias?.failureCode ?? row.send;
  return {
    terminalResult:
      alias?.terminalResult ??
      (context === 'stale-generation' ? row.replyTerminalAlias : undefined) ??
      row.sendTerminal ??
      WIRE_TERMINAL_RESULT_BY_FAILURE_CODE.get(failureCode)!,
    failureCode
  };
}

/** Decodes the stateful reply failureCode convention without exposing wire offsets to callers. */
export function internalFrameworkErrorKindFromWireFailureCode(
  failureCode: number,
  context: FailureContext = 'reply'
): ZLinkFrameworkInternalErrorKind | undefined {
  return RECEIVE_MAPPING.get(context)!.get(failureCode);
}

/** Restores a typed failure only when its wire terminal result is canonical. */
export function internalFrameworkErrorKindFromWireReply(
  terminalResult: number,
  failureCode: number
): ZLinkFrameworkInternalErrorKind | undefined {
  const alias = REPLY_TERMINAL_ALIASES.get(terminalResult)?.get(failureCode);
  if (alias !== undefined) return alias;
  const kind = internalFrameworkErrorKindFromWireFailureCode(failureCode);
  if (kind === undefined) return undefined;
  const expectedTerminalResult = WIRE_TERMINAL_RESULT_BY_FAILURE_CODE.get(failureCode);
  return expectedTerminalResult === terminalResult ? kind : undefined;
}

const BOUNDARY_WIRE_TERMINAL_RESULTS = new Set<number>(ServiceWireBoundaryTerminalResults);

/** Checks the terminal and failure-code pair before a transport maps it. */
export function isCanonicalWireReplyTerminal(terminalResult: number, failureCode: number): boolean {
  if (terminalResult === RequestResult.Ok)
    return failureCode === ServiceWireFrameworkErrorCode.none;
  if (BOUNDARY_WIRE_TERMINAL_RESULTS.has(terminalResult)) return failureCode === 0;
  return internalFrameworkErrorKindFromWireReply(terminalResult, failureCode) !== undefined;
}
