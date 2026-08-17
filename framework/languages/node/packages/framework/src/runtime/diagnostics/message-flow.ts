import {
  SpanStatusCode,
  trace as openTelemetryTrace,
  type Attributes
} from '@opentelemetry/api';
import { logs as openTelemetryLogs, SeverityNumber } from '@opentelemetry/api-logs';
import {
  MESSAGE_FLOW_MODE_RANK,
  type ZLinkDiagnosticsOptions,
  type ZLinkFlowOrigin,
  type ZLinkMessageFlowLogMode
} from '../../contracts';
import {
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind,
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  type ZLinkRuntimeMessageFlowResult,
  type ZLinkRuntimeMessageFlowEvent
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { ZLinkDispatchErrorSink } from './dispatch-error-port';
import { currentFlowContext } from './flow-context';

export interface ZLinkMessageFlowModeCell {
  mode: ZLinkMessageFlowLogMode;
}

export interface ZLinkDiagnosticsContext {
  readonly diagnostics: ZLinkDiagnosticsOptions;
  readonly liveMode: ZLinkMessageFlowModeCell;
  readonly sourceMeshGeneration: bigint;
}

export const DEFAULT_ZLINK_DIAGNOSTICS: ZLinkDiagnosticsOptions = {
  messageFlow: 'errors',
  sampleRate: 1,
  includeMessageSizes: false
};

const telemetryLogger = openTelemetryLogs.getLogger('@zlink-systems/framework');
const telemetryTracer = openTelemetryTrace.getTracer('@zlink-systems/framework');
let diagnosticsGeneration = 0n;

interface ZLinkTelemetryRecord {
  readonly eventId: 'zlink.message_flow' | 'zlink.dispatch_error';
  readonly timestamp: Date;
  readonly phase?: string;
  readonly outcome: ZLinkRuntimeMessageFlowResult;
  readonly surface: 'node' | 'channel' | 'spot' | 'instance_spot' | 'actor' | 'stream'
    | 'actor_relocation' | 'classic_fanout';
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
  readonly commandId?: number;
  readonly messageSizeBytes?: number;
  readonly durationSeconds?: number;
  readonly errorType?: string;
  readonly errorMessage?: string;
  readonly errorCauseType?: string;
  readonly errorCauseMessage?: string;
}

export function effectiveMessageFlow(ctx: ZLinkDiagnosticsContext): ZLinkMessageFlowLogMode {
  return ctx.liveMode.mode;
}

export function createMessageFlowModeCell(
  dispatch: { diagnostics?: ZLinkDiagnosticsOptions } | undefined
): ZLinkMessageFlowModeCell {
  return { mode: dispatch?.diagnostics?.messageFlow ?? 'errors' };
}

export function createDiagnosticsContext(
  dispatch: { diagnostics?: ZLinkDiagnosticsOptions } | undefined,
  _providerResolver: unknown,
  liveMode: ZLinkMessageFlowModeCell
): ZLinkDiagnosticsContext {
  return {
    diagnostics: dispatch?.diagnostics ?? DEFAULT_ZLINK_DIAGNOSTICS,
    liveMode,
    sourceMeshGeneration: ++diagnosticsGeneration
  };
}

function requiredMode(
  outcome: ZLinkMessageFlowOutcome,
  result?: ZLinkRuntimeMessageFlowResult
): ZLinkMessageFlowLogMode {
  return outcome === ZLinkMessageFlowOutcome.Dropped
    || outcome === ZLinkMessageFlowOutcome.Backpressured
    || outcome === ZLinkMessageFlowOutcome.Error
    || (result !== undefined && result !== 'succeeded')
    ? 'errors'
    : 'normal';
}

export function flowIfEnabled(
  flow: ZLinkMessageFlowTracer | undefined,
  outcome: ZLinkMessageFlowOutcome,
  result?: ZLinkRuntimeMessageFlowResult,
  flowId?: string,
  sourceMeshGeneration?: bigint | string
): ZLinkMessageFlowTracePoint | undefined {
  if (flow === undefined) return undefined;
  return typeof flow.begin === 'function'
    ? flow.begin(outcome, result, flowId, sourceMeshGeneration)
    : flow.accepts(outcome) ? flow : undefined;
}

export interface ZLinkMessageFlowTracePoint {
  trace(flowInput: MessageFlowInput): void;
}

type MessageFlowInput = Omit<
  ZLinkRuntimeMessageFlowEvent,
  'effectiveMode' | 'flowId' | 'flowOrigin'
> & {
  readonly effectiveMode?: ZLinkMessageFlowLogMode;
  readonly flowId?: string;
  readonly flowOrigin?: import('../../contracts').ZLinkFlowOrigin;
};

export class ZLinkMessageFlowTracer {
  private tracedEvents = 0;
  private providerFailures = 0;
  private localSamplingSequence = 0n;
  private errorsTracePoint?: ZLinkMessageFlowTracePoint;
  private normalTracePoint?: ZLinkMessageFlowTracePoint;
  private detailedTracePoint?: ZLinkMessageFlowTracePoint;

  constructor(
    private readonly ctx: ZLinkDiagnosticsContext,
    private readonly errorSink: ZLinkDispatchErrorSink
  ) {}

  enabled(outcome: ZLinkMessageFlowOutcome): boolean {
    const mode = effectiveMessageFlow(this.ctx);
    return MESSAGE_FLOW_MODE_RANK[mode] >= MESSAGE_FLOW_MODE_RANK[requiredMode(outcome)];
  }

  accepts(outcome: ZLinkMessageFlowOutcome): boolean {
    return this.enabled(outcome);
  }

  begin(
    outcome: ZLinkMessageFlowOutcome,
    result?: ZLinkRuntimeMessageFlowResult,
    flowId?: string,
    sourceMeshGeneration?: bigint | string
  ): ZLinkMessageFlowTracePoint | undefined {
    const mode = effectiveMessageFlow(this.ctx);
    if (MESSAGE_FLOW_MODE_RANK[mode] < MESSAGE_FLOW_MODE_RANK[requiredMode(outcome, result)]) {
      return undefined;
    }
    const samplingFlowId = flowId ?? currentFlowContext()?.flowId;
    if (!this.acceptsSample(outcome, result, samplingFlowId, sourceMeshGeneration)) {
      return undefined;
    }
    switch (mode) {
      case 'errors':
        return this.errorsTracePoint ??= {
          trace: flowInput => this.traceAcceptedAtMode(flowInput, 'errors')
        };
      case 'normal':
        return this.normalTracePoint ??= {
          trace: flowInput => this.traceAcceptedAtMode(flowInput, 'normal')
        };
      case 'detailed':
        return this.detailedTracePoint ??= {
          trace: flowInput => this.traceAcceptedAtMode(flowInput, 'detailed')
        };
      case 'off':
        return undefined;
    }
  }

  flowCreationEnabled(): boolean {
    return effectiveMessageFlow(this.ctx) !== 'off';
  }

  trace(flowInput: MessageFlowInput): void {
    const effectiveMode = effectiveMessageFlow(this.ctx);
    if (
      MESSAGE_FLOW_MODE_RANK[effectiveMode]
      < MESSAGE_FLOW_MODE_RANK[requiredMode(flowInput.outcome, flowInput.result)]
    ) return;
    const ambient = currentFlowContext();
    const flowId = flowInput.flowId ?? ambient?.flowId;
    if (!this.acceptsSample(
      flowInput.outcome,
      flowInput.result,
      flowId,
      flowInput.sourceMeshGeneration
    )) return;
    this.traceAcceptedAtMode(flowInput, effectiveMode, flowId, ambient?.flowOrigin);
  }

  private traceAcceptedAtMode(
    flowInput: MessageFlowInput,
    effectiveMode: ZLinkMessageFlowLogMode,
    acceptedFlowId?: string,
    acceptedFlowOrigin?: ZLinkFlowOrigin
  ): void {
    const ambient = acceptedFlowId === undefined ? currentFlowContext() : undefined;
    const flowId = flowInput.flowId ?? acceptedFlowId ?? ambient?.flowId;
    const flowOrigin = flowInput.flowOrigin ?? acceptedFlowOrigin ?? ambient?.flowOrigin;
    const flow: ZLinkRuntimeMessageFlowEvent = {
      ...flowInput,
      ...(flowId === undefined ? {} : { flowId, flowOrigin }),
      effectiveMode
    };
    this.tracedEvents += 1;
    this.publish(toTelemetryRecord(
      flow,
      effectiveMode === 'detailed' && this.ctx.diagnostics.includeMessageSizes
        ? flow.messageSize
        : undefined
    ));
  }

  private acceptsSample(
    outcome: ZLinkMessageFlowOutcome,
    result: ZLinkRuntimeMessageFlowResult | undefined,
    flowId: string | undefined,
    sourceMeshGeneration: bigint | string | undefined
  ): boolean {
    return outcome === ZLinkMessageFlowOutcome.Dropped
      || outcome === ZLinkMessageFlowOutcome.Backpressured
      || outcome === ZLinkMessageFlowOutcome.Error
      || (result !== undefined && result !== 'succeeded')
      || this.sample(flowId, sourceMeshGeneration);
  }

  get tracedCount(): number {
    return this.tracedEvents;
  }

  get providerFailureCount(): number {
    return this.providerFailures;
  }

  private sample(flowId: string | undefined, sourceMeshGeneration: bigint | string | undefined): boolean {
    const rate = this.ctx.diagnostics.sampleRate;
    if (rate >= 1.0) return true;
    if (rate <= 0.0) return false;
    const samplingKey = flowId
      ?? `${sourceMeshGeneration ?? this.ctx.sourceMeshGeneration}:${this.nextSamplingSequence()}`;
    return hashFlowId(samplingKey) / 0x1_0000_0000 < rate;
  }

  private nextSamplingSequence(): bigint {
    this.localSamplingSequence += 1n;
    return this.localSamplingSequence;
  }

  private publish(record: ZLinkTelemetryRecord): void {
    this.publishToLogger(record);
    this.publishToTrace(record);
  }

  private publishToLogger(record: ZLinkTelemetryRecord): void {
    try {
      const severityNumber = record.eventId === 'zlink.dispatch_error'
        ? SeverityNumber.ERROR
        : SeverityNumber.INFO;
      telemetryLogger.emit({
        eventName: record.eventId,
        timestamp: record.timestamp,
        severityNumber,
        severityText: record.eventId === 'zlink.dispatch_error' ? 'ERROR' : 'INFO',
        body: structuredLogBody(record),
        attributes: structuredLogAttributes(record)
      });
    } catch (error) {
      this.reportProviderFailure('logger-provider', error);
    }
  }

  private publishToTrace(record: ZLinkTelemetryRecord): void {
    try {
      const span = telemetryTracer.startSpan(record.eventId, {
        attributes: traceAttributes(record)
      });
      if (record.eventId === 'zlink.dispatch_error') {
        span.setStatus({ code: SpanStatusCode.ERROR, message: record.reason });
      }
      span.end();
    } catch (error) {
      this.reportProviderFailure('trace-provider', error);
    }
  }

  private reportProviderFailure(source: string, error: unknown): void {
    this.providerFailures += 1;
    try {
      this.errorSink.reportRuntimeTaskException(source, error);
    } catch {
      // The fallback diagnostics path is isolated for the same reason.
    }
  }
}

function toTelemetryRecord(
  flow: ZLinkRuntimeMessageFlowEvent,
  messageSizeBytes: number | undefined
): ZLinkTelemetryRecord {
  return {
    eventId: flow.outcome === ZLinkMessageFlowOutcome.Error
      ? 'zlink.dispatch_error'
      : 'zlink.message_flow',
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
    flowOrigin: flow.flowOrigin,
    spotId: flow.spotId,
    instanceSpotType: flow.instanceSpotType,
    activationState: flow.activationState,
    actorId: flow.actorId,
    commandId: flow.commandId,
    messageSizeBytes,
    durationSeconds: flow.durationSeconds,
    errorType: flow.errorType,
    errorMessage: boundedText(flow.errorMessage),
    errorCauseType: flow.errorCauseType,
    errorCauseMessage: boundedText(flow.errorCauseMessage)
  };
}

function traceAttributes(record: ZLinkTelemetryRecord): Attributes {
  const attributes: Attributes = {};
  for (const [name, value] of Object.entries(record)) {
    if (value === undefined || name === 'timestamp') continue;
    attributes[toSnakeCase(name)] = value instanceof Date ? value.toISOString() : value;
  }
  return attributes;
}

function structuredLogAttributes(record: ZLinkTelemetryRecord): Attributes {
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
    corr: record.correlationId,
    flow: record.flowId,
    origin: record.flowOrigin,
    outcome: record.outcome,
    reason: record.reason,
    size: record.messageSizeBytes
  });
}

function structuredLogBody(record: ZLinkTelemetryRecord): string {
  return `zlink flow: ${Object.entries(structuredLogAttributes(record))
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
    case ZLinkMessageFlowOutcome.Received: return 'received';
    case ZLinkMessageFlowOutcome.Admitted: return 'admitted';
    case ZLinkMessageFlowOutcome.Dispatched: return 'dispatched';
    case ZLinkMessageFlowOutcome.Completed: return 'completed';
    case ZLinkMessageFlowOutcome.Replied: return 'replied';
    case ZLinkMessageFlowOutcome.Dropped: return 'dropped';
    case ZLinkMessageFlowOutcome.Sent: return 'sent';
    case ZLinkMessageFlowOutcome.ReplyReceived: return 'reply_received';
    case ZLinkMessageFlowOutcome.Backpressured: return 'backpressured';
    case ZLinkMessageFlowOutcome.Error: return undefined;
  }
}

function messageFlowOutcome(
  outcome: ZLinkMessageFlowOutcome
): ZLinkTelemetryRecord['outcome'] {
  if (outcome === ZLinkMessageFlowOutcome.Error) return 'failed';
  if (outcome === ZLinkMessageFlowOutcome.Backpressured) return 'backpressured';
  if (outcome === ZLinkMessageFlowOutcome.Dropped) return 'dropped';
  return 'succeeded';
}

function messageFlowSurface(
  surface: ZLinkDispatchErrorSurface
): ZLinkTelemetryRecord['surface'] {
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

function messageFlowKind(
  kind: ZLinkDispatchMessageKind
): ZLinkTelemetryRecord['messageKind'] {
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

function hashFlowId(flowId: string): number {
  let hash = 0x811c9dc5;
  for (let index = 0; index < flowId.length; index += 1) {
    hash ^= flowId.charCodeAt(index);
    hash = Math.imul(hash, 0x01000193);
  }
  return hash >>> 0;
}

function boundedText(value: string | undefined): string | undefined {
  return value === undefined ? undefined : value.slice(0, 512);
}

function toSnakeCase(value: string): string {
  return value.replace(/[A-Z]/g, (letter) => `_${letter.toLowerCase()}`);
}
