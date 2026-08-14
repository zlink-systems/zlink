import type { ZLinkFlowOrigin } from './ZLinkFlowOrigin';

export enum ZLinkCoreHwmProfile {
  Compact = 'compact',
  LowLatency = 'low_latency',
  Balanced = 'balanced',
  Throughput = 'throughput'
}

export enum ZLinkApplicationJobQueueProfile {
  Compact = 'compact',
  LowLatency = 'low_latency',
  Balanced = 'balanced',
  Throughput = 'throughput'
}

export interface ZLinkInboundDispatchOptions {
  coreHwmMemoryLimitBytes(value: bigint | undefined): this;
  coreHwmBudgetBytes(value: bigint | undefined): this;
  coreHwmProfile(value: ZLinkCoreHwmProfile): this;
  applicationJobQueueProfile(value: ZLinkApplicationJobQueueProfile): this;
  maxQueuedApplicationJobs(value: bigint | undefined): this;
}

export interface ZLinkDispatchOptions {
  readonly unhandled: ZLinkUnhandledDispatchOptions;
  readonly diagnostics: ZLinkDiagnosticsOptions;
}

export interface ZLinkDispatchOptionsBuilder {
  messageFlow(mode: ZLinkMessageFlowLogMode): this;
  traceSampleRate(rate: number): this;
  includeMessageSizes(include: boolean): this;
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
}

export enum ZLinkUnhandledDispatchAction {
  ReplyError = 'replyError',
  LogAndDrop = 'logAndDrop',
  Drop = 'drop',
  Throw = 'throw'
}

export type ZLinkMessageFlowLogMode = 'off' | 'errors' | 'normal' | 'detailed';

export const MESSAGE_FLOW_MODE_RANK: Record<ZLinkMessageFlowLogMode, number> = {
  off: 0,
  errors: 1,
  normal: 2,
  detailed: 3
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
