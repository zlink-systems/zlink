import { appendFileSync, mkdirSync } from 'node:fs';
import { dirname } from 'node:path';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import {
  MESSAGE_FLOW_MODE_RANK,
  ZLinkMessageFlowLogMode
} from '../../contracts';
import type {
  ZLinkDiagnosticsOptions,
  ZLinkMessageFlowEvent,
  ZLinkMessageFlowObserver,
  ZLinkRuntimeErrorSink,
  Type
} from '../../contracts';
import {
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind,
  ZLinkMessageFlowPhase,
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  type ZLinkDispatchFailure,
  type ZLinkRuntimeMessageFlowEvent
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { ZLinkDispatchErrorSink } from './dispatch-error-port';
import { currentOrCreateFlow } from './flow-context';
import {
  getDispatchObserverType,
  getRuntimeErrorSinkType
} from '../../contracts/Configuration/DispatchObserverRegistration';
import type { ZLinkRuntimeMetrics } from './runtime-metrics';

/** Shared, runtime-mutable message-flow mode cell (the C++ live_mode). */
export interface ZLinkMessageFlowModeCell {
  mode: ZLinkMessageFlowLogMode;
}

/**
 * Diagnostics state shared by the flow tracer and the dispatch error reporter: the
 * configured diagnostics, the live-mode cell (so set_message_flow_mode flips every
 * surface), the optional flow observer type, and the provider resolver.
 */
export interface ZLinkDiagnosticsContext {
  readonly diagnostics: ZLinkDiagnosticsOptions;
  readonly liveMode: ZLinkMessageFlowModeCell;
  readonly messageFlowObserverType?: Type<ZLinkMessageFlowObserver>;
  readonly runtimeErrorSinkType?: Type<ZLinkRuntimeErrorSink>;
  readonly providerResolver?: ZLinkProviderResolver;
}

export const DEFAULT_ZLINK_DIAGNOSTICS: ZLinkDiagnosticsOptions = {
  messageFlow: ZLinkMessageFlowLogMode.ErrorsOnly,
  sampleRate: 1,
  includeMessageSizes: false
};

export function effectiveMessageFlow(ctx: ZLinkDiagnosticsContext): ZLinkMessageFlowLogMode {
  return ctx.liveMode.mode;
}

/** Create the shared live-mode cell, seeded from the configured mode (default errorsOnly). */
export function createMessageFlowModeCell(
  dispatch: { diagnostics?: ZLinkDiagnosticsOptions } | undefined
): ZLinkMessageFlowModeCell {
  return { mode: dispatch?.diagnostics?.messageFlow ?? ZLinkMessageFlowLogMode.ErrorsOnly };
}

/** Assemble the diagnostics context the reporter + tracer share. */
export function createDiagnosticsContext(
  dispatch:
    | {
        diagnostics?: ZLinkDiagnosticsOptions;
      }
    | undefined,
  providerResolver: ZLinkProviderResolver | undefined,
  liveMode: ZLinkMessageFlowModeCell
): ZLinkDiagnosticsContext {
  return {
    diagnostics: dispatch?.diagnostics ?? DEFAULT_ZLINK_DIAGNOSTICS,
    liveMode,
    messageFlowObserverType: getDispatchObserverType(dispatch as import('../../contracts').ZLinkDispatchOptions | undefined),
    runtimeErrorSinkType: getRuntimeErrorSinkType(
      dispatch as import('../../contracts').ZLinkDispatchOptions | undefined
    ),
    providerResolver
  };
}

function requiredMode(outcome: ZLinkMessageFlowOutcome): ZLinkMessageFlowLogMode {
  return outcome === ZLinkMessageFlowOutcome.Dropped
    || outcome === ZLinkMessageFlowOutcome.Error
    ? ZLinkMessageFlowLogMode.ErrorsOnly
    : ZLinkMessageFlowLogMode.KeyTransitions;
}

/**
 * Returns the tracer only when this outcome is enabled, so call sites read as
 * `flowIfEnabled(reporter?.flow, outcome)?.trace({ ...event })`. Optional chaining
 * short-circuits, so the event object literal is never built when tracing is off —
 * keeping the disabled path allocation-free.
 */
export function flowIfEnabled(
  flow: ZLinkMessageFlowTracer | undefined,
  outcome: ZLinkMessageFlowOutcome
): ZLinkMessageFlowTracer | undefined {
  return flow !== undefined && flow.accepts(outcome) ? flow : undefined;
}

/**
 * Success-path message-flow tracer — the twin of ZLinkDispatchErrorReporter for
 * received/dispatched/replied/sent/replyReceived transitions, keyed by correlation id.
 * Mirrors the C++/.NET/Java tracer. Build the event only after enabled(outcome) so an
 * "off" dispatch pays nothing but a mode read.
 */
export class ZLinkMessageFlowTracer {
  private tracedEvents = 0;
  private observerFailures = 0;
  private observerRunning = false;
  private readonly observerQueue: ZLinkMessageFlowEvent[] = [];
  private observerQueueHead = 0;

  constructor(
    private readonly ctx: ZLinkDiagnosticsContext,
    private readonly errorSink: ZLinkDispatchErrorSink,
    private readonly metrics?: ZLinkRuntimeMetrics,
    private readonly observerQueueCapacity = 1024
  ) {}

  enabled(outcome: ZLinkMessageFlowOutcome): boolean {
    return MESSAGE_FLOW_MODE_RANK[effectiveMessageFlow(this.ctx)] >= MESSAGE_FLOW_MODE_RANK[requiredMode(outcome)];
  }

  accepts(outcome: ZLinkMessageFlowOutcome): boolean {
    return this.enabled(outcome) || this.ctx.messageFlowObserverType !== undefined;
  }

  flowCreationEnabled(): boolean {
    return effectiveMessageFlow(this.ctx) !== ZLinkMessageFlowLogMode.Off;
  }

  trace(flowInput: Omit<ZLinkRuntimeMessageFlowEvent, 'effectiveMode' | 'flowId' | 'flowOrigin'> & {
    readonly effectiveMode?: ZLinkMessageFlowLogMode;
    readonly flowId?: string;
    readonly flowOrigin?: import('../../contracts').ZLinkFlowOrigin;
  }, defaultLogLevel: 'error' | 'warn' | 'debug' = 'error'): void {
    const traceEnabled = this.enabled(flowInput.outcome);
    if (!traceEnabled && this.ctx.messageFlowObserverType === undefined) {
      return;
    }
    const root = flowInput.flowId !== undefined && flowInput.flowOrigin !== undefined
      ? { flowId: flowInput.flowId, flowOrigin: flowInput.flowOrigin }
      : currentOrCreateFlow();
    const flow: ZLinkRuntimeMessageFlowEvent = {
      ...flowInput,
      ...root,
      effectiveMode: flowInput.effectiveMode ?? effectiveMessageFlow(this.ctx)
    };
    if (traceEnabled && (
      flow.outcome === ZLinkMessageFlowOutcome.Dropped
      || flow.outcome === ZLinkMessageFlowOutcome.Error
      || this.sample(flow.flowId)
    )) {
      this.tracedEvents += 1;
      try {
        this.logDefault(flow, defaultLogLevel);
      } catch (error) {
        this.errorSink.reportRuntimeTaskException('message-flow', error);
      }
    }

    this.enqueueObserver(flow);
  }

  get tracedCount(): number {
    return this.tracedEvents;
  }

  get observerFailureCount(): number {
    return this.observerFailures;
  }

  private enqueueObserver(flow: ZLinkRuntimeMessageFlowEvent): void {
    if (this.ctx.messageFlowObserverType === undefined) return;
    if (
      this.observerRunning
      && this.observerQueue.length - this.observerQueueHead >= this.observerQueueCapacity
    ) {
      this.metrics?.count('zlink.observability.events.overflow', 1, { source: flow.outcome });
      return;
    }
    this.observerQueue.push(toPublicMessageFlowEvent(flow));
    if (this.observerRunning) return;
    this.observerRunning = true;
    queueMicrotask(() => { void this.drainObserverQueue(); });
  }

  private async drainObserverQueue(): Promise<void> {
    const observerType = this.ctx.messageFlowObserverType;
    if (observerType === undefined) {
      this.observerQueue.length = 0;
      this.observerQueueHead = 0;
      this.observerRunning = false;
      return;
    }
    while (this.observerQueueHead < this.observerQueue.length) {
      const flow = this.observerQueue[this.observerQueueHead++]!;
      try {
        const observer = await this.resolveObserver(observerType);
        await observer.onMessageFlow(flow);
      } catch (error) {
        this.observerFailures += 1;
        await this.reportObserverFailure(error);
      }
      if (this.observerQueueHead >= 1024 && this.observerQueueHead * 2 >= this.observerQueue.length) {
        this.observerQueue.splice(0, this.observerQueueHead);
        this.observerQueueHead = 0;
      }
    }
    this.observerQueue.length = 0;
    this.observerQueueHead = 0;
    this.observerRunning = false;
  }

  private sample(flowId: string): boolean {
    const rate = this.ctx.diagnostics.sampleRate;
    if (rate >= 1.0) {
      return true;
    }
    if (rate <= 0.0) {
      return false;
    }
    return hashFlowId(flowId) / 0x1_0000_0000 < rate;
  }

  private logDefault(flow: ZLinkRuntimeMessageFlowEvent, level: 'error' | 'warn' | 'debug'): void {
    const d = this.ctx.diagnostics;
    const includeSize =
      flow.messageSize !== undefined &&
      MESSAGE_FLOW_MODE_RANK[effectiveMessageFlow(this.ctx)] >= MESSAGE_FLOW_MODE_RANK[ZLinkMessageFlowLogMode.Verbose] &&
      d.includeMessageSizes !== false;
    const line = flowLine(flow, d.label, includeSize ? flow.messageSize : undefined);
    if (d.logFile !== undefined) {
      writeTraceFile(d.logFile, line);
    } else {
      console[level](line);
    }
  }

  private async resolveObserver(
    observerType: Type<ZLinkMessageFlowObserver>
  ): Promise<ZLinkMessageFlowObserver> {
    const existing = this.ctx.providerResolver?.get?.(observerType);
    if (existing !== undefined) {
      return existing;
    }
    const created = await this.ctx.providerResolver?.create?.(observerType);
    if (created !== undefined) {
      return created;
    }
    return new observerType();
  }

  private async reportObserverFailure(error: unknown): Promise<void> {
    const sinkType = this.ctx.runtimeErrorSinkType;
    if (sinkType === undefined) {
      this.errorSink.reportRuntimeTaskException('message-flow-observer', error);
      return;
    }
    try {
      const existing = this.ctx.providerResolver?.get?.(sinkType);
      const created = existing ?? await this.ctx.providerResolver?.create?.(sinkType);
      const sink = created ?? new sinkType();
      await sink.onRuntimeError({
        eventId: 'zlink.runtime_error',
        timestamp: new Date(),
        kind: 'observer_failed',
        source: 'message_flow_observer',
        reason: boundedErrorReason(error)
      });
    } catch (sinkError) {
      this.errorSink.reportRuntimeTaskException('runtime-error-sink', sinkError);
    }
  }
}

function boundedErrorReason(error: unknown): string {
  const name = error instanceof Error ? error.name : 'Error';
  const message = error instanceof Error ? error.message : String(error);
  return `${name}: ${message}`.slice(0, 512);
}

function toPublicMessageFlowEvent(flow: ZLinkRuntimeMessageFlowEvent): ZLinkMessageFlowEvent {
  const failed = flow.outcome === ZLinkMessageFlowOutcome.Error;
  return {
    eventId: failed ? 'zlink.dispatch_error' : 'zlink.message_flow',
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
    messageSizeBytes: flow.messageSize
  };
}

function messageFlowPhase(outcome: ZLinkMessageFlowOutcome): ZLinkMessageFlowPhase | undefined {
  switch (outcome) {
    case ZLinkMessageFlowOutcome.Received:
      return ZLinkMessageFlowPhase.Received;
    case ZLinkMessageFlowOutcome.Dispatched:
      return ZLinkMessageFlowPhase.Dispatched;
    case ZLinkMessageFlowOutcome.Replied:
      return ZLinkMessageFlowPhase.Replied;
    case ZLinkMessageFlowOutcome.Dropped:
      return ZLinkMessageFlowPhase.Dropped;
    case ZLinkMessageFlowOutcome.Sent:
      return ZLinkMessageFlowPhase.Sent;
    case ZLinkMessageFlowOutcome.ReplyReceived:
      return ZLinkMessageFlowPhase.ReplyReceived;
    case ZLinkMessageFlowOutcome.Error:
      return undefined;
  }
}

function messageFlowOutcome(
  outcome: ZLinkMessageFlowOutcome
): ZLinkMessageFlowEvent['outcome'] {
  if (outcome === ZLinkMessageFlowOutcome.Error) return 'failed';
  if (outcome === ZLinkMessageFlowOutcome.Dropped) return 'dropped';
  return 'succeeded';
}

function messageFlowSurface(
  surface: ZLinkDispatchErrorSurface
): ZLinkMessageFlowEvent['surface'] {
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
): ZLinkMessageFlowEvent['messageKind'] {
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

function field(name: string, value: string | undefined): string | undefined {
  return value === undefined || value === '' ? undefined : `${name}=${value}`;
}

export function flowLine(
  flow: ZLinkRuntimeMessageFlowEvent,
  label: string | undefined,
  size: number | undefined
): string {
  return [
    'message flow',
    `phase=${flow.outcome}`,
    `surface=${flow.surface}`,
    `kind=${flow.messageKind}`,
    field('label', label),
    field('packet', flow.packetName),
    field('channel', flow.channelName),
    field('mesh', flow.meshName),
    field('topic', flow.topic),
    field('corr', flow.correlationId),
    field('flow', flow.flowId),
    field('origin', flow.flowOrigin),
    field('src', flow.sourceRid),
    field('target', flow.targetRid),
    field('spot', flow.spotId),
    field('instance_type', flow.instanceSpotType),
    field('activation_state', flow.activationState),
    field('actor', flow.actorId),
    field('errorReason', flow.errorReason),
    field('errorAction', flow.errorAction),
    field('errorType', flow.errorType),
    field('errorMessage', flow.errorMessage),
    size === undefined ? undefined : `size=${size}`
  ]
    .filter((value): value is string => value !== undefined)
    .join(' ');
}

export function errorLine(event: ZLinkDispatchFailure, label: string | undefined): string {
  return [
    'dispatch error',
    `surface=${event.surface}`,
    `kind=${event.messageKind}`,
    `reason=${event.reason}`,
    `action=${event.action}`,
    field('label', label),
    field('packet', event.packetName),
    field('channel', event.channelName),
    field('mesh', event.meshName),
    field('topic', event.topic),
    field('corr', event.correlationId),
    field('src', event.sourceRid),
    field('target', event.targetRid),
    field('spot', event.spotId),
    field('instance_type', event.instanceSpotType),
    field('activation_state', event.activationState),
    field('actor', event.actorId),
    field('errorType', event.errorType),
    field('errorMessage', event.errorMessage)
  ]
    .filter((value): value is string => value !== undefined)
    .join(' ');
}

export function writeTraceFile(path: string, line: string): void {
  try {
    const dir = dirname(path);
    if (dir.length > 0) {
      mkdirSync(dir, { recursive: true });
    }
    appendFileSync(path, `${line}\n`);
  } catch {
    // best-effort: tracing must never break dispatch.
  }
}
