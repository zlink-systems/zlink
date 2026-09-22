import {
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind,
  type ZLinkDispatchFailure
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { ZLinkRuntimeMetrics } from '../diagnostics';
import {
  ZLinkMessageFlowTracer,
  DEFAULT_ZLINK_DIAGNOSTICS,
  type ZLinkDiagnosticsContext
} from '../diagnostics';
import type { ZLinkDispatchErrorSink } from '../diagnostics/dispatch-error-port';
import { dispatchErrorDetails } from '../diagnostics/dispatch-error-details';

export type { ZLinkDispatchErrorSink } from '../diagnostics/dispatch-error-port';

type ZLinkRuntimeDispatchFailure = ZLinkDispatchFailure & {
  readonly error?: unknown;
};

export class ZLinkDispatchErrorReporter {
  private reportedEvents = 0;
  /**
   * Success-path tracer companion: every surface already receives a reporter, so
   * exposing the flow tracer here wires all dispatch sites without threading a new
   * parameter. Shares the same diagnostics context (live mode) and error sink.
   */
  readonly flow: ZLinkMessageFlowTracer;

  constructor(
    _observerType: undefined,
    _providerResolver: unknown,
    errorSink: ZLinkDispatchErrorSink,
    ctx?: ZLinkDiagnosticsContext,
    private readonly metrics?: ZLinkRuntimeMetrics
  ) {
    const flowCtx: ZLinkDiagnosticsContext = ctx ?? {
      diagnostics: DEFAULT_ZLINK_DIAGNOSTICS,
      liveMode: { mode: 'errors' },
      sourceMeshGeneration: 0n
    };
    this.flow = new ZLinkMessageFlowTracer(flowCtx, errorSink);
  }

  report(event: ZLinkRuntimeDispatchFailure): void {
    const dropReason = channelDropReason(event);
    if (dropReason !== undefined) {
      this.metrics?.count('zlink.mesh_node.messages.dropped', 1, {
        surface: event.surface,
        message_kind: event.messageKind,
        reason: dropReason
      });
    }
    const tracePoint = this.flow.begin(ZLinkMessageFlowOutcome.Error);
    if (tracePoint === undefined) return;
    const normalized = normalizeDispatchFailure(event);
    const errorInfo = dispatchErrorDetails(normalized.error);
    this.reportedEvents += 1;
    tracePoint.trace({
      outcome: ZLinkMessageFlowOutcome.Error,
      surface: normalized.surface,
      messageKind: normalized.messageKind,
      packetName: normalized.packetName,
      channelName: normalized.channelName,
      channelRouteKind: normalized.channelRouteKind,
      meshName: normalized.meshName,
      topic: normalized.topic,
      correlationId: normalized.correlationId,
      flowId: normalized.flowId,
      flowOrigin: normalized.flowOrigin,
      sourceRid: normalized.sourceRid,
      targetRid: normalized.targetRid,
      serverRid: normalized.serverRid,
      spotId: normalized.spotId,
      instanceSpotType: normalized.instanceSpotType,
      activationState: normalized.activationState,
      actorId: normalized.actorId,
      commandId: normalized.commandId,
      errorReason: normalized.reason,
      errorAction: normalized.action,
      errorType: errorInfo.errorType,
      errorMessage: errorInfo.errorMessage
    });
  }

  get reportedCount(): number {
    return this.reportedEvents;
  }

  get providerFailureCount(): number {
    return this.flow.providerFailureCount;
  }
}

function normalizeDispatchFailure(event: ZLinkRuntimeDispatchFailure): ZLinkRuntimeDispatchFailure {
  if (event.messageKind !== 'publish') return event;
  return {
    ...event,
    surface: ZLinkDispatchErrorSurface.ClassicFanout,
    messageKind: ZLinkDispatchMessageKind.Send,
    channelRouteKind: undefined
  };
}

function channelDropReason(event: ZLinkRuntimeDispatchFailure): string | undefined {
  if (
    event.action !== ZLinkDispatchErrorAction.Drop ||
    (event.surface !== ZLinkDispatchErrorSurface.Channel &&
      event.surface !== ZLinkDispatchErrorSurface.RouteMeshChannel)
  )
    return undefined;
  switch (event.reason) {
    case ZLinkDispatchErrorReason.HandlerMissing:
      return 'no_handler';
    case ZLinkDispatchErrorReason.PayloadDecodeFailed:
    case ZLinkDispatchErrorReason.InvalidFrame:
      return 'decode_error';
    default:
      return undefined;
  }
}
