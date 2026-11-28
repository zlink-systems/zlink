import type { ZLinkFlowOrigin } from '../../contracts/Dispatch/ZLinkFlowOrigin';
import {
  MESSAGE_FLOW_MODE_RANK,
  type ZLinkDiagnosticsOptions,
  type ZLinkMessageFlowLogMode,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind,
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  type ZLinkRuntimeMessageFlowResult
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import { dispatchErrorDetails } from './dispatch-error-details';
import { currentFlowContext } from './flow-context';
import { toTelemetryRecord, type MessageFlowInput } from './message-flow-record';
import {
  messageFlowDelivery,
  openTelemetryLease,
  telemetryProviderFailureCount,
  type ZLinkTelemetryLease
} from './message-flow-delivery';

export interface ZLinkMessageFlowModeCell {
  mode: ZLinkMessageFlowLogMode;
}

export interface ZLinkDiagnosticsContext {
  readonly diagnostics: ZLinkDiagnosticsOptions;
  readonly liveMode: ZLinkMessageFlowModeCell;
  readonly sourceMeshGeneration: bigint;
  readonly telemetryLease?: ZLinkTelemetryLease;
}

export const DEFAULT_ZLINK_DIAGNOSTICS: ZLinkDiagnosticsOptions = {
  messageFlow: 'errors',
  sampleRate: 1,
  includeMessageSizes: false
};

let diagnosticsGeneration = 0n;

export function effectiveMessageFlow(ctx: ZLinkDiagnosticsContext): ZLinkMessageFlowLogMode {
  return ctx.liveMode.mode;
}

/**
 * Message-flow tracing is on at Normal and Detailed only (04-flow-correlation
 * §4). Below it no flow ID is created, validated, captured or propagated. This
 * is the one decision every flow processing point reads.
 */
export function messageFlowTracingEnabled(mode: ZLinkMessageFlowLogMode): boolean {
  return MESSAGE_FLOW_MODE_RANK[mode] >= MESSAGE_FLOW_MODE_RANK.normal;
}

export function createMessageFlowModeCell(
  dispatch: { diagnostics?: ZLinkDiagnosticsOptions } | undefined
): ZLinkMessageFlowModeCell {
  return { mode: dispatch?.diagnostics?.messageFlow ?? 'errors' };
}

export function createDiagnosticsContext(
  dispatch: { diagnostics?: ZLinkDiagnosticsOptions } | undefined,
  _providerResolver: unknown,
  liveMode: ZLinkMessageFlowModeCell,
  telemetryLease: ZLinkTelemetryLease = openTelemetryLease()
): ZLinkDiagnosticsContext {
  return {
    diagnostics: dispatch?.diagnostics ?? DEFAULT_ZLINK_DIAGNOSTICS,
    liveMode,
    sourceMeshGeneration: ++diagnosticsGeneration,
    telemetryLease
  };
}

function requiredMode(
  outcome: ZLinkMessageFlowOutcome,
  result?: ZLinkRuntimeMessageFlowResult
): ZLinkMessageFlowLogMode {
  //  succeeded가 아닌 terminal은 Errors level에서도 보여야 한다 — 기록 범위 설정
  //  절의 level 규칙.
  return outcome === ZLinkMessageFlowOutcome.Dropped ||
    outcome === ZLinkMessageFlowOutcome.Backpressured ||
    outcome === ZLinkMessageFlowOutcome.Error ||
    (result !== undefined && result !== 'succeeded')
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
    : flow.accepts(outcome)
      ? flow
      : undefined;
}

export interface ZLinkMessageFlowTracePoint {
  trace(flowInput: MessageFlowInput): void;
}

export function traceSessionBindingControlDrop(
  flow: ZLinkMessageFlowTracer | undefined,
  sourceRid: string,
  actorId: string,
  packetName: string,
  errorReason: 'location_unavailable' | 'stale_target',
  error?: unknown
): void {
  const point = flowIfEnabled(flow, ZLinkMessageFlowOutcome.Dropped);
  if (point === undefined) return;
  point.trace({
    outcome: ZLinkMessageFlowOutcome.Dropped,
    surface: ZLinkDispatchErrorSurface.StreamSession,
    messageKind: ZLinkDispatchMessageKind.Control,
    sourceRid,
    actorId,
    packetName,
    errorReason,
    ...dispatchErrorDetails(error)
  });
}

export class ZLinkMessageFlowTracer {
  private tracedEvents = 0;
  private readonly telemetryLease: ZLinkTelemetryLease;
  private localSamplingSequence = 0n;
  private errorsTracePoint?: ZLinkMessageFlowTracePoint;
  private normalTracePoint?: ZLinkMessageFlowTracePoint;
  private detailedTracePoint?: ZLinkMessageFlowTracePoint;

  constructor(private readonly ctx: ZLinkDiagnosticsContext) {
    this.telemetryLease = ctx.telemetryLease ?? openTelemetryLease();
  }

  enabled(outcome: ZLinkMessageFlowOutcome): boolean {
    const mode = effectiveMessageFlow(this.ctx);
    return (
      MESSAGE_FLOW_MODE_RANK[mode] >= MESSAGE_FLOW_MODE_RANK[requiredMode(outcome)] &&
      messageFlowDelivery().accepts(this.telemetryLease)
    );
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
    if (!messageFlowDelivery().accepts(this.telemetryLease)) return undefined;
    const samplingFlowId = messageFlowTracingEnabled(mode)
      ? (flowId ?? currentFlowContext()?.flowId)
      : undefined;
    if (!this.acceptsSample(outcome, result, samplingFlowId, sourceMeshGeneration)) {
      return undefined;
    }
    switch (mode) {
      case 'errors':
        return (this.errorsTracePoint ??= {
          trace: (flowInput) => this.traceAcceptedAtMode(flowInput, 'errors')
        });
      case 'normal':
        return (this.normalTracePoint ??= {
          trace: (flowInput) => this.traceAcceptedAtMode(flowInput, 'normal')
        });
      case 'detailed':
        return (this.detailedTracePoint ??= {
          trace: (flowInput) => this.traceAcceptedAtMode(flowInput, 'detailed')
        });
      case 'off':
        return undefined;
    }
  }

  flowCreationEnabled(): boolean {
    return messageFlowTracingEnabled(effectiveMessageFlow(this.ctx));
  }

  trace(flowInput: MessageFlowInput): void {
    const effectiveMode = effectiveMessageFlow(this.ctx);
    if (
      MESSAGE_FLOW_MODE_RANK[effectiveMode] <
      MESSAGE_FLOW_MODE_RANK[requiredMode(flowInput.outcome, flowInput.result)]
    )
      return;
    if (!messageFlowDelivery().accepts(this.telemetryLease)) return;
    const tracingEnabled = messageFlowTracingEnabled(effectiveMode);
    const ambient = tracingEnabled ? currentFlowContext() : undefined;
    const flowId = tracingEnabled ? (flowInput.flowId ?? ambient?.flowId) : undefined;
    if (
      !this.acceptsSample(
        flowInput.outcome,
        flowInput.result,
        flowId,
        flowInput.sourceMeshGeneration
      )
    )
      return;
    this.traceAcceptedAtMode(flowInput, effectiveMode, flowId, ambient?.flowOrigin);
  }

  private traceAcceptedAtMode(
    flowInput: MessageFlowInput,
    effectiveMode: ZLinkMessageFlowLogMode,
    acceptedFlowId?: string,
    acceptedFlowOrigin?: ZLinkFlowOrigin
  ): void {
    if (!messageFlowDelivery().accepts(this.telemetryLease)) return;
    const tracingEnabled = messageFlowTracingEnabled(effectiveMode);
    const ambient = tracingEnabled ? currentFlowContext() : undefined;
    const flowId = tracingEnabled
      ? (flowInput.flowId ?? acceptedFlowId ?? ambient?.flowId)
      : undefined;
    const flowOrigin = tracingEnabled
      ? (flowInput.flowOrigin ?? acceptedFlowOrigin ?? ambient?.flowOrigin)
      : undefined;
    const flow: MessageFlowInput = {
      ...flowInput,
      ...(tracingEnabled
        ? flowId === undefined
          ? {}
          : { flowId, flowOrigin }
        : { flowId: undefined, flowOrigin: undefined }),
      streamSessionId: flowInput.streamSessionId ?? ambient?.streamSessionId,
      effectiveMode
    };
    this.tracedEvents += 1;
    messageFlowDelivery().submit(
      this.telemetryLease,
      toTelemetryRecord(
        flow,
        effectiveMode === 'detailed' && this.ctx.diagnostics.includeMessageSizes
          ? flow.messageSize
          : undefined,
        // Spec 26 §4: terminal elapsed time is a Detailed-level addition; the
        // other languages strip it below that level.
        effectiveMode === 'detailed' ? flow.durationSeconds : undefined
      )
    );
  }

  private acceptsSample(
    outcome: ZLinkMessageFlowOutcome,
    result: ZLinkRuntimeMessageFlowResult | undefined,
    flowId: string | undefined,
    sourceMeshGeneration: bigint | string | undefined
  ): boolean {
    return (
      outcome === ZLinkMessageFlowOutcome.Dropped ||
      outcome === ZLinkMessageFlowOutcome.Backpressured ||
      outcome === ZLinkMessageFlowOutcome.Error ||
      (result !== undefined && result !== 'succeeded') ||
      this.sample(flowId, sourceMeshGeneration)
    );
  }

  get tracedCount(): number {
    return this.tracedEvents;
  }

  get providerFailureCount(): number {
    return telemetryProviderFailureCount();
  }

  private sample(
    flowId: string | undefined,
    sourceMeshGeneration: bigint | string | undefined
  ): boolean {
    const rate = this.ctx.diagnostics.sampleRate;
    if (rate >= 1.0) return true;
    if (rate <= 0.0) return false;
    const samplingKey =
      flowId ??
      `${sourceMeshGeneration ?? this.ctx.sourceMeshGeneration}:${this.nextSamplingSequence()}`;
    return hashFlowId(samplingKey) / 0x1_0000_0000 < rate;
  }

  private nextSamplingSequence(): bigint {
    this.localSamplingSequence += 1n;
    return this.localSamplingSequence;
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
