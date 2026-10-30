import type { Attributes } from '@opentelemetry/api';
import type { ZLinkMessageFlowLogMode } from '../../contracts';
import {
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind,
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  type ZLinkRuntimeMessageFlowEvent,
  type ZLinkRuntimeMessageFlowResult
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import { ERROR_MESSAGE_MAX_LENGTH } from './dispatch-error-details';
import { FLOW_ORIGIN_VALUES } from './flow-context';
export const TELEMETRY_INSTRUMENTATION_NAME = '@zlink-systems/framework';
export const MESSAGE_FLOW_EVENT_ID = 'zlink.message_flow';
export const DISPATCH_ERROR_EVENT_ID = 'zlink.dispatch_error';

export interface ZLinkTelemetryRecord {
  readonly severity?: 'warning';
  readonly eventId: typeof MESSAGE_FLOW_EVENT_ID | typeof DISPATCH_ERROR_EVENT_ID;
  readonly timestamp: Date;
  readonly phase?: string;
  readonly outcome: ZLinkRuntimeMessageFlowResult;
  readonly surface:
    | 'node'
    | 'channel'
    | 'spot'
    | 'instance_spot'
    | 'actor'
    | 'stream'
    | 'actor_relocation'
    | 'classic_fanout';
  readonly messageKind: 'request' | 'send' | 'response' | 'error' | 'control';
  readonly reason?: string;
  readonly action?: string;
  readonly packetName?: string;
  readonly channelName?: string;
  readonly channelRouteKind?: 'route_mesh' | 'client_server';
  readonly meshName?: string;
  readonly topic?: string;
  readonly correlationId?: string;
  readonly sourceRid?: string;
  readonly targetRid?: string;
  readonly serverRid?: string;
  readonly flowId?: string;
  readonly flowOrigin?: string;
  readonly spotId?: string;
  readonly instanceSpotType?: string;
  readonly activationState?: string;
  readonly actorId?: string;
  readonly streamSessionId?: string;
  readonly commandId?: number;
  readonly messageSizeBytes?: number;
  readonly durationSeconds?: number;
  readonly errorType?: string;
  readonly errorMessage?: string;
  readonly errorCauseType?: string;
  readonly errorCauseMessage?: string;
}

export type MessageFlowInput = Omit<
  ZLinkRuntimeMessageFlowEvent,
  'effectiveMode' | 'flowId' | 'flowOrigin'
> & {
  readonly severity?: 'warning';
  readonly effectiveMode?: ZLinkMessageFlowLogMode;
  readonly flowId?: string;
  readonly flowOrigin?: import('../../contracts').ZLinkFlowOrigin;
};

export function toTelemetryRecord(
  flow: MessageFlowInput,
  messageSizeBytes: number | undefined,
  durationSeconds: number | undefined
): ZLinkTelemetryRecord {
  return {
    severity: flow.severity,
    eventId:
      flow.outcome === ZLinkMessageFlowOutcome.Error
        ? DISPATCH_ERROR_EVENT_ID
        : MESSAGE_FLOW_EVENT_ID,
    timestamp: new Date(),
    phase: messageFlowPhase(flow.outcome),
    outcome: flow.result ?? messageFlowOutcome(flow.outcome),
    surface: messageFlowSurface(flow.surface),
    messageKind: messageFlowKind(flow.messageKind),
    reason: flow.errorReason,
    action: flow.errorAction,
    packetName: flow.packetName,
    channelName: flow.channelName,
    channelRouteKind: flow.channelRouteKind ?? channelRouteKind(flow.surface),
    meshName: flow.meshName,
    topic: flow.topic,
    correlationId: flow.correlationId,
    sourceRid: flow.sourceRid,
    targetRid: flow.targetRid,
    serverRid: flow.serverRid,
    flowId: flow.flowId,
    flowOrigin: flow.flowOrigin === undefined ? undefined : FLOW_ORIGIN_VALUES[flow.flowOrigin],
    spotId: flow.spotId,
    instanceSpotType: flow.instanceSpotType,
    activationState: flow.activationState,
    actorId: flow.actorId,
    streamSessionId: flow.streamSessionId,
    commandId: flow.commandId,
    messageSizeBytes,
    durationSeconds,
    errorType: flow.errorType,
    errorMessage: flow.errorMessage,
    errorCauseType: flow.errorCauseType,
    errorCauseMessage: boundedText(flow.errorCauseMessage)
  };
}

export function traceAttributes(record: ZLinkTelemetryRecord): Attributes {
  const attributes: Attributes = {};
  for (const [name, value] of Object.entries(record)) {
    if (value === undefined || name === 'timestamp' || name === 'severity') continue;
    attributes[toSnakeCase(name)] = value instanceof Date ? value.toISOString() : value;
  }
  return attributes;
}

export function structuredLogAttributes(record: ZLinkTelemetryRecord): Attributes {
  //  기록의 attribute 이름은 Message flow tracing 3.2가 고정한다. 로그 본문의
  //  `zlink flow:` 축약 key(`kind`, `mesh` …)와 다른 집합이다 —
  //  structuredLogBodyAttributes를 참고.
  return compactAttributes({
    event_id: record.eventId,
    timestamp: record.timestamp.toISOString(),
    phase: record.phase,
    surface: record.surface,
    message_kind: record.messageKind,
    mesh_name: record.meshName,
    channel_name: record.channelName,
    channel_route_kind: record.channelRouteKind,
    source_rid: record.sourceRid,
    target_rid: record.targetRid,
    server_rid: record.serverRid,
    packet_name: record.packetName,
    topic: record.topic,
    spot_id: record.spotId,
    instance_spot_type: record.instanceSpotType,
    activation_state: record.activationState,
    actor_id: record.actorId,
    stream_session_id: record.streamSessionId,
    correlation_id: record.correlationId,
    flow_id: record.flowId,
    flow_origin: record.flowOrigin,
    outcome: record.outcome,
    reason: record.reason,
    action: record.action,
    command_id: record.commandId,
    error_type: record.errorType,
    error_message: record.errorMessage,
    error_cause_type: record.errorCauseType,
    error_cause_message: record.errorCauseMessage,
    message_size_bytes: record.messageSizeBytes,
    duration_seconds: record.durationSeconds
  });
}

function structuredLogBodyAttributes(record: ZLinkTelemetryRecord): Attributes {
  //  `zlink flow:` 본문 전용 축약 key — Message flow tracing "Structured log 대체 표기".
  return compactAttributes({
    event: record.eventId,
    phase: record.phase,
    surface: record.surface,
    kind: record.messageKind,
    mesh: record.meshName,
    channel: record.channelName,
    channel_route: record.channelRouteKind,
    source_rid: record.sourceRid,
    target_rid: record.targetRid,
    server_rid: record.serverRid,
    packet: record.packetName,
    topic: record.topic,
    spot: record.spotId,
    instance_type: record.instanceSpotType,
    activation_state: record.activationState,
    actor: record.actorId,
    session: record.streamSessionId,
    corr: record.correlationId,
    flow: record.flowId,
    origin: record.flowOrigin,
    outcome: record.outcome,
    reason: record.reason,
    action: record.action,
    error_type: record.errorType,
    error_message: record.errorMessage,
    size: record.messageSizeBytes
  });
}

export function structuredLogBody(record: ZLinkTelemetryRecord): string {
  return `zlink flow: ${Object.entries(structuredLogBodyAttributes(record))
    .map(([key, value]) => `${key}=${String(value)}`)
    .join(' ')}`;
}

function compactAttributes(values: Record<string, string | number | undefined>): Attributes {
  const attributes: Attributes = {};
  for (const [key, value] of Object.entries(values)) {
    if (value !== undefined) attributes[key] = value;
  }
  return attributes;
}

function messageFlowPhase(outcome: ZLinkMessageFlowOutcome): string | undefined {
  switch (outcome) {
    case ZLinkMessageFlowOutcome.Received:
      return 'received';
    case ZLinkMessageFlowOutcome.Admitted:
      return 'admitted';
    case ZLinkMessageFlowOutcome.Dispatched:
      return 'dispatched';
    case ZLinkMessageFlowOutcome.Completed:
      return 'completed';
    case ZLinkMessageFlowOutcome.Replied:
      return 'replied';
    case ZLinkMessageFlowOutcome.Dropped:
      return 'dropped';
    case ZLinkMessageFlowOutcome.Sent:
      return 'sent';
    case ZLinkMessageFlowOutcome.ReplyReceived:
      return 'reply_received';
    case ZLinkMessageFlowOutcome.Backpressured:
      return 'backpressured';
    case ZLinkMessageFlowOutcome.Error:
      return undefined;
  }
}

function messageFlowOutcome(outcome: ZLinkMessageFlowOutcome): ZLinkTelemetryRecord['outcome'] {
  if (outcome === ZLinkMessageFlowOutcome.Error) return 'failed';
  if (outcome === ZLinkMessageFlowOutcome.Backpressured) return 'backpressured';
  if (outcome === ZLinkMessageFlowOutcome.Dropped) return 'dropped';
  return 'succeeded';
}

function messageFlowSurface(surface: ZLinkDispatchErrorSurface): ZLinkTelemetryRecord['surface'] {
  switch (surface) {
    case ZLinkDispatchErrorSurface.Node:
      return 'node';
    case ZLinkDispatchErrorSurface.Channel:
    case ZLinkDispatchErrorSurface.RouteMeshChannel:
      return 'channel';
    case ZLinkDispatchErrorSurface.SpotRoute:
    case ZLinkDispatchErrorSurface.SpotSubscription:
      return 'spot';
    case ZLinkDispatchErrorSurface.InstanceSpot:
      return 'instance_spot';
    case ZLinkDispatchErrorSurface.SpotActor:
      return 'actor';
    case ZLinkDispatchErrorSurface.StreamSession:
      return 'stream';
    case ZLinkDispatchErrorSurface.ActorRelocation:
      return 'actor_relocation';
    case ZLinkDispatchErrorSurface.ClassicFanout:
      return 'classic_fanout';
  }
}

function messageFlowKind(kind: ZLinkDispatchMessageKind): ZLinkTelemetryRecord['messageKind'] {
  switch (kind) {
    case ZLinkDispatchMessageKind.Request:
    case ZLinkDispatchMessageKind.ActorRequest:
      return 'request';
    case ZLinkDispatchMessageKind.Send:
    case ZLinkDispatchMessageKind.ActorSend:
      return 'send';
    case ZLinkDispatchMessageKind.Response:
      return 'response';
    case ZLinkDispatchMessageKind.Error:
      return 'error';
    case ZLinkDispatchMessageKind.Control:
      return 'control';
    case ZLinkDispatchMessageKind.Publish:
      throw new Error('Classic fanout publish must not create a normal message-flow record.');
  }
}

function channelRouteKind(
  surface: ZLinkDispatchErrorSurface
): ZLinkTelemetryRecord['channelRouteKind'] {
  if (surface === ZLinkDispatchErrorSurface.RouteMeshChannel) return 'route_mesh';
  if (surface === ZLinkDispatchErrorSurface.Channel) return 'client_server';
  return undefined;
}

function boundedText(value: string | undefined): string | undefined {
  return value === undefined ? undefined : value.slice(0, ERROR_MESSAGE_MAX_LENGTH);
}

function toSnakeCase(value: string): string {
  return value.replace(/[A-Z]/g, (letter) => `_${letter.toLowerCase()}`);
}
