import type { RoutingId, SpotId, Type } from '../Common';
import type { ZLinkFlowOrigin } from './ZLinkFlowOrigin';
import type {
  ZLinkMessageKind,
  ZLinkMessageSurface,
  ZLinkRuntimeErrorSink
} from '../RouteMesh';

export interface ZLinkDispatchOptions {
  readonly unhandled: ZLinkUnhandledDispatchOptions;
  readonly diagnostics: ZLinkDiagnosticsOptions;
}

export interface ZLinkDispatchOptionsBuilder {
  setMessageFlowObserver(observerType: Type<ZLinkMessageFlowObserver>): this;
  setRuntimeErrorSink(sinkType: Type<ZLinkRuntimeErrorSink>): this;

  /** Fluent diagnostics/tracing config (builder-chain only). */
  messageFlow(mode: ZLinkMessageFlowLogMode): this;

  traceSampleRate(rate: number): this;

  includeMessageSizes(include: boolean): this;

  /** Send tracing/error logs to a dedicated file (separated from app logs). */
  traceLogFile(path: string): this;

  /** Human-readable label stamped on every trace line (label=) for aggregation. */
  traceLabel(label: string): this;
}

/**
 * A message lifecycle outcome. Error outcomes carry the dispatch error fields on the
 * same typed event used by the success path.
 */
export enum ZLinkRuntimeMessageFlowOutcome {
  Received = 'received',
  Dispatched = 'dispatched',
  Replied = 'replied',
  Dropped = 'dropped',
  Sent = 'sent',
  ReplyReceived = 'replyReceived',
  Error = 'error'
}

export enum ZLinkMessageFlowPhase {
  Received = 'received',
  Admitted = 'admitted',
  Dispatched = 'dispatched',
  Completed = 'completed',
  Replied = 'replied',
  Sent = 'sent',
  ReplyReceived = 'reply_received',
  Backpressured = 'backpressured',
  Dropped = 'dropped'
}

export interface ZLinkMessageFlowEvent {
  readonly eventId: 'zlink.message_flow' | 'zlink.dispatch_error';
  readonly timestamp: Date;
  readonly phase?: ZLinkMessageFlowPhase;
  readonly outcome: ZLinkMessageFlowOutcome;
  readonly surface: ZLinkMessageSurface;
  readonly messageKind: ZLinkMessageKind;
  readonly reason?: ZLinkDispatchErrorReason;
  readonly action?: ZLinkDispatchErrorAction;
  readonly packetName?: string;
  readonly channelName?: string;
  readonly meshName?: string;
  readonly topic?: string;
  readonly correlationId?: string;
  readonly sourceRid?: RoutingId;
  readonly targetRid?: RoutingId;
  readonly flowId?: string;
  readonly flowOrigin?: ZLinkFlowOrigin;
  readonly spotId?: SpotId;
  readonly instanceSpotType?: string;
  readonly activationState?: 'activating' | 'ready' | 'closing';
  readonly actorId?: string;
  readonly messageSizeBytes?: number;
  readonly durationSeconds?: number;
}

export type ZLinkMessageFlowOutcome =
  | 'succeeded'
  | 'failed'
  | 'backpressured'
  | 'dropped'
  | 'cancelled'
  | 'shutdown';

export type ZLinkMessageFlowReason =
  | 'backpressure'
  | 'stale_target'
  | 'target_closed'
  | 'shutdown'
  | 'location_unavailable'
  | 'activation_rejected'
  | 'activation_timeout';

export interface ZLinkRuntimeMessageFlowEvent {
  readonly outcome: ZLinkRuntimeMessageFlowOutcome;
  readonly surface: ZLinkDispatchErrorSurface;
  readonly messageKind: ZLinkDispatchMessageKind;
  readonly packetName?: string;
  readonly channelName?: string;
  readonly meshName?: string;
  readonly topic?: string;
  readonly correlationId?: string;
  readonly sourceRid?: string;
  readonly targetRid?: string;
  readonly peerRid?: string;
  readonly socketRole?: string;
  readonly effectiveMode: ZLinkMessageFlowLogMode;
  readonly flowId: string;
  readonly flowOrigin: ZLinkFlowOrigin;
  readonly spotId?: string;
  readonly instanceSpotType?: string;
  readonly activationState?: 'activating' | 'ready' | 'closing';
  readonly actorId?: string;
  readonly messageSize?: number;
  readonly errorReason?: ZLinkDispatchErrorReason;
  readonly errorAction?: ZLinkDispatchErrorAction;
  readonly errorType?: string;
  readonly errorMessage?: string;
}

export interface ZLinkMessageFlowObserver {
  onMessageFlow(flow: ZLinkMessageFlowEvent): Promise<void> | void;
}

/**
 * Resolve from the runtime to turn message-flow tracing on/off (or change verbosity)
 * at runtime without a restart. Read live by every dispatch surface.
 */
export interface ZLinkMessageFlowControl {
  setMessageFlowMode(mode: ZLinkMessageFlowLogMode): void;
  messageFlowMode(): ZLinkMessageFlowLogMode;
}

export interface ZLinkDispatchFailure {
  readonly surface: ZLinkDispatchErrorSurface;
  readonly messageKind: ZLinkDispatchMessageKind;
  readonly reason: ZLinkDispatchErrorReason;
  readonly action: ZLinkDispatchErrorAction;
  readonly packetName?: string;
  readonly channelName?: string;
  readonly meshName?: string;
  readonly topic?: string;
  readonly spotId?: string;
  readonly instanceSpotType?: string;
  readonly activationState?: 'activating' | 'ready' | 'closing';
  readonly actorId?: string;
  readonly sourceRid?: string;
  readonly targetRid?: string;
  readonly correlationId?: string;
  readonly flowId?: string;
  readonly flowOrigin?: ZLinkFlowOrigin;
  readonly errorType?: string;
  readonly errorMessage?: string;
}

export interface ZLinkUnhandledDispatchOptions {
  request: ZLinkUnhandledDispatchAction;
  send: ZLinkUnhandledDispatchAction;
  publish: ZLinkUnhandledDispatchAction;
}

export interface ZLinkDiagnosticsOptions {
  messageFlow: ZLinkMessageFlowLogMode;
  sampleRate: number;
  includeMessageSizes: boolean;
  /** When set, tracing/error logs go to this dedicated file (separated from app logs). */
  logFile?: string;
  /** Human-readable runtime label stamped on each trace line. */
  label?: string;
}

export enum ZLinkUnhandledDispatchAction {
  ReplyError = 'replyError',
  LogAndDrop = 'logAndDrop',
  Drop = 'drop',
  Throw = 'throw'
}

export enum ZLinkMessageFlowLogMode {
  Off = 'off',
  ErrorsOnly = 'errorsOnly',
  KeyTransitions = 'keyTransitions',
  Verbose = 'verbose'
}

/** Severity rank for the mode ladder (off < errorsOnly < keyTransitions < verbose < diagnostic). */
export const MESSAGE_FLOW_MODE_RANK: Record<ZLinkMessageFlowLogMode, number> = {
  [ZLinkMessageFlowLogMode.Off]: 0,
  [ZLinkMessageFlowLogMode.ErrorsOnly]: 1,
  [ZLinkMessageFlowLogMode.KeyTransitions]: 2,
  [ZLinkMessageFlowLogMode.Verbose]: 3
};

export enum ZLinkDispatchErrorSurface {
  Channel = 'channel',
  RouteMeshChannel = 'routeMeshChannel',
  SpotRoute = 'spotRoute',
  InstanceSpot = 'instance_spot',
  SpotSubscription = 'spotSubscription',
  SpotActor = 'spotActor',
  StreamSession = 'streamSession'
}

export enum ZLinkDispatchMessageKind {
  Request = 'request',
  Send = 'send',
  Publish = 'publish',
  Response = 'response',
  Error = 'error',
  ActorRequest = 'actorRequest',
  ActorSend = 'actorSend'
}

export type ZLinkDispatchErrorReason =
  | 'no_handler'
  | 'decode_error'
  | 'handler_exception'
  | 'invalid_frame'
  | 'reply_path_missing'
  | 'unexpected_reply'
  | 'backpressure'
  | 'stale_target'
  | 'shutdown';

export type ZLinkDispatchErrorAction = 'reply_error' | 'fail_caller' | 'drop';

export enum ZLinkRuntimeDispatchErrorReason {
  HandlerMissing = 'no_handler',
  PayloadDecodeFailed = 'decode_error',
  HandlerException = 'handler_exception',
  InvalidFrame = 'invalid_frame',
  ReplyPathMissing = 'reply_path_missing',
  UnexpectedReply = 'unexpected_reply',
  Backpressure = 'backpressure',
  StaleTarget = 'stale_target',
  Shutdown = 'shutdown'
}

export enum ZLinkRuntimeDispatchErrorAction {
  ReplyError = 'reply_error',
  FailCaller = 'fail_caller',
  Drop = 'drop'
}
