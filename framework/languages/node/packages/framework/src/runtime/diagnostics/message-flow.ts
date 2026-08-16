import {
  SpanStatusCode,
  trace as openTelemetryTrace,
  type Attributes
} from '@opentelemetry/api';
import { logs as openTelemetryLogs, SeverityNumber } from '@opentelemetry/api-logs';
import {
  MESSAGE_FLOW_MODE_RANK,
  type ZLinkDiagnosticsOptions,
  type ZLinkMessageFlowLogMode
} from '../../contracts';
import {
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind,
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  type ZLinkRuntimeMessageFlowEvent
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { ZLinkDispatchErrorSink } from './dispatch-error-port';
import { currentFlowContext, currentOrCreateFlow } from './flow-context';

export interface ZLinkMessageFlowModeCell {
  mode: ZLinkMessageFlowLogMode;
}

export interface ZLinkDiagnosticsContext {
  readonly diagnostics: ZLinkDiagnosticsOptions;
  readonly liveMode: ZLinkMessageFlowModeCell;
}

export const DEFAULT_ZLINK_DIAGNOSTICS: ZLinkDiagnosticsOptions = {
  messageFlow: 'errors',
  sampleRate: 1,
  includeMessageSizes: false
};

const telemetryLogger = openTelemetryLogs.getLogger('@zlink-systems/framework');
const telemetryTracer = openTelemetryTrace.getTracer('@zlink-systems/framework');

interface ZLinkTelemetryRecord {
  readonly eventId: 'zlink.message_flow' | 'zlink.dispatch_error';
  readonly timestamp: Date;
  readonly phase?: string;
  readonly outcome: 'succeeded' | 'failed' | 'dropped';
  readonly surface: 'channel' | 'spot' | 'instance_spot' | 'actor' | 'stream';
  readonly messageKind: 'request' | 'send' | 'publish' | 'response' | 'error';
  readonly reason?: string;
  readonly action?: string;
  readonly packetName?: string;
  readonly channelName?: string;
  readonly meshName?: string;
  readonly topic?: string;
  readonly correlationId?: string;
  readonly sourceRid?: string;
  readonly targetRid?: string;
  readonly flowId: string;
  readonly flowOrigin: string;
  readonly spotId?: string;
  readonly instanceSpotType?: string;
  readonly activationState?: string;
  readonly actorId?: string;
  readonly commandId?: number;
  readonly messageSizeBytes?: number;
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
    liveMode
  };
}

function requiredMode(outcome: ZLinkMessageFlowOutcome): ZLinkMessageFlowLogMode {
  return outcome === ZLinkMessageFlowOutcome.Dropped
    || outcome === ZLinkMessageFlowOutcome.Error
    ? 'errors'
    : 'normal';
}

export function flowIfEnabled(
  flow: ZLinkMessageFlowTracer | undefined,
  outcome: ZLinkMessageFlowOutcome
): ZLinkMessageFlowTracer | undefined {
  return flow !== undefined && flow.accepts(outcome) ? flow : undefined;
}

export class ZLinkMessageFlowTracer {
  private tracedEvents = 0;
  private providerFailures = 0;
  private readonly modeByFlow = new WeakMap<object, ZLinkMessageFlowLogMode>();

  constructor(
    private readonly ctx: ZLinkDiagnosticsContext,
    private readonly errorSink: ZLinkDispatchErrorSink
  ) {}

  enabled(outcome: ZLinkMessageFlowOutcome): boolean {
    const ambient = currentFlowContext();
    const mode = ambient === undefined
      ? effectiveMessageFlow(this.ctx)
      : this.modeByFlow.get(ambient) ?? effectiveMessageFlow(this.ctx);
    return MESSAGE_FLOW_MODE_RANK[mode] >= MESSAGE_FLOW_MODE_RANK[requiredMode(outcome)];
  }

  accepts(outcome: ZLinkMessageFlowOutcome): boolean {
    return this.enabled(outcome);
  }

  flowCreationEnabled(): boolean {
    return effectiveMessageFlow(this.ctx) !== 'off';
  }

  /**
   * Traces an event built only when the outcome is enabled.
   *
   * Use this on failure and other rare transitions: the thunk keeps the call
   * site free of an explicit gate and never runs when tracing is off, at the
   * cost of one closure per call. Hot paths that trace every message must keep
   * the `if (enabled(outcome))` guard so no closure is allocated there either.
   */
  traceLazy(
    outcome: ZLinkMessageFlowOutcome,
    build: () => Omit<ZLinkRuntimeMessageFlowEvent, 'effectiveMode' | 'flowId' | 'flowOrigin'> & {
      readonly effectiveMode?: ZLinkMessageFlowLogMode;
      readonly flowId?: string;
      readonly flowOrigin?: import('../../contracts').ZLinkFlowOrigin;
    }
  ): void {
    if (!this.enabled(outcome)) return;
    this.trace(build());
  }

  trace(flowInput: Omit<ZLinkRuntimeMessageFlowEvent, 'effectiveMode' | 'flowId' | 'flowOrigin'> & {
    readonly effectiveMode?: ZLinkMessageFlowLogMode;
    readonly flowId?: string;
    readonly flowOrigin?: import('../../contracts').ZLinkFlowOrigin;
  }): void {
    const ambient = currentFlowContext();
    const effectiveMode = flowInput.effectiveMode
      ?? (ambient === undefined ? undefined : this.modeByFlow.get(ambient))
      ?? effectiveMessageFlow(this.ctx);
    if (ambient !== undefined && !this.modeByFlow.has(ambient)) {
      this.modeByFlow.set(ambient, effectiveMode);
    }
    if (
      MESSAGE_FLOW_MODE_RANK[effectiveMode]
      < MESSAGE_FLOW_MODE_RANK[requiredMode(flowInput.outcome)]
    ) return;
    const root = flowInput.flowId !== undefined && flowInput.flowOrigin !== undefined
      ? { flowId: flowInput.flowId, flowOrigin: flowInput.flowOrigin }
      : currentOrCreateFlow();
    const flow: ZLinkRuntimeMessageFlowEvent = {
      ...flowInput,
      ...root,
      effectiveMode
    };
    if (
      flow.outcome !== ZLinkMessageFlowOutcome.Dropped
      && flow.outcome !== ZLinkMessageFlowOutcome.Error
      && !this.sample(flow.flowId)
    ) return;

    this.tracedEvents += 1;
    this.publish(toTelemetryRecord(
      flow,
      effectiveMode === 'detailed' && this.ctx.diagnostics.includeMessageSizes
        ? flow.messageSize
        : undefined
    ));
  }

  get tracedCount(): number {
    return this.tracedEvents;
  }

  get providerFailureCount(): number {
    return this.providerFailures;
  }

  private sample(flowId: string): boolean {
    const rate = this.ctx.diagnostics.sampleRate;
    if (rate >= 1.0) return true;
    if (rate <= 0.0) return false;
    return hashFlowId(flowId) / 0x1_0000_0000 < rate;
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
        body: record.eventId,
        attributes: telemetryAttributes(record)
      });
    } catch (error) {
      this.reportProviderFailure('logger-provider', error);
    }
  }

  private publishToTrace(record: ZLinkTelemetryRecord): void {
    try {
      const span = telemetryTracer.startSpan(record.eventId, {
        attributes: telemetryAttributes(record)
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
    outcome: messageFlowOutcome(flow.outcome),
    surface: messageFlowSurface(flow.surface),
    messageKind: messageFlowKind(flow.messageKind),
    reason: flow.errorReason,
    action: flow.errorAction,
    packetName: flow.packetName,
    channelName: flow.channelName,
    meshName: flow.meshName,
    topic: flow.topic,
    correlationId: flow.correlationId,
    sourceRid: flow.sourceRid,
    targetRid: flow.targetRid,
    flowId: flow.flowId,
    flowOrigin: flow.flowOrigin,
    spotId: flow.spotId,
    instanceSpotType: flow.instanceSpotType,
    activationState: flow.activationState,
    actorId: flow.actorId,
    commandId: flow.commandId,
    messageSizeBytes,
    errorType: flow.errorType,
    errorMessage: boundedText(flow.errorMessage),
    errorCauseType: flow.errorCauseType,
    errorCauseMessage: boundedText(flow.errorCauseMessage)
  };
}

function telemetryAttributes(record: ZLinkTelemetryRecord): Attributes {
  const attributes: Attributes = {};
  for (const [name, value] of Object.entries(record)) {
    if (value === undefined || name === 'timestamp') continue;
    attributes[toSnakeCase(name)] = value instanceof Date ? value.toISOString() : value;
  }
  return attributes;
}

function messageFlowPhase(outcome: ZLinkMessageFlowOutcome): string | undefined {
  switch (outcome) {
    case ZLinkMessageFlowOutcome.Received: return 'received';
    case ZLinkMessageFlowOutcome.Dispatched: return 'dispatched';
    case ZLinkMessageFlowOutcome.Replied: return 'replied';
    case ZLinkMessageFlowOutcome.Dropped: return 'dropped';
    case ZLinkMessageFlowOutcome.Sent: return 'sent';
    case ZLinkMessageFlowOutcome.ReplyReceived: return 'reply_received';
    case ZLinkMessageFlowOutcome.Error: return undefined;
  }
}

function messageFlowOutcome(
  outcome: ZLinkMessageFlowOutcome
): ZLinkTelemetryRecord['outcome'] {
  if (outcome === ZLinkMessageFlowOutcome.Error) return 'failed';
  if (outcome === ZLinkMessageFlowOutcome.Dropped) return 'dropped';
  return 'succeeded';
}

function messageFlowSurface(
  surface: ZLinkDispatchErrorSurface
): ZLinkTelemetryRecord['surface'] {
  switch (surface) {
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
    case ZLinkDispatchMessageKind.Publish:
      return 'publish';
    case ZLinkDispatchMessageKind.Response:
      return 'response';
    case ZLinkDispatchMessageKind.Error:
      return 'error';
  }
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
