import {
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchErrorSurface,
  type ZLinkDispatchFailure
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { ZLinkRuntimeMetrics } from '../diagnostics';
import {
  ZLinkMessageFlowTracer,
  DEFAULT_ZLINK_DIAGNOSTICS,
  type ZLinkDiagnosticsContext
} from '../diagnostics';
import type { ZLinkDispatchErrorSink } from '../diagnostics/dispatch-error-port';

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
      liveMode: { mode: 'errors' }
    };
    this.flow = new ZLinkMessageFlowTracer(flowCtx, errorSink);
  }

  report(event: ZLinkRuntimeDispatchFailure): void {
    const errorInfo = dispatchErrorInfo(event);
    this.reportedEvents += 1;
    const dropReason = channelDropReason(event);
    if (dropReason !== undefined) {
      this.metrics?.count('zlink.mesh_node.messages.dropped', 1, {
        surface: event.surface,
        message_kind: event.messageKind,
        reason: dropReason
      });
    }
    this.flow.trace({
      outcome: ZLinkMessageFlowOutcome.Error,
      surface: event.surface,
      messageKind: event.messageKind,
      packetName: event.packetName,
      channelName: event.channelName,
      meshName: event.meshName,
      topic: event.topic,
      correlationId: event.correlationId,
      flowId: event.flowId,
      flowOrigin: event.flowOrigin,
      sourceRid: event.sourceRid,
      targetRid: event.targetRid,
      spotId: event.spotId,
      instanceSpotType: event.instanceSpotType,
      activationState: event.activationState,
      actorId: event.actorId,
      errorReason: event.reason,
      errorAction: event.action,
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

function channelDropReason(event: ZLinkRuntimeDispatchFailure): string | undefined {
  if (
    event.action !== ZLinkDispatchErrorAction.Drop
    || (event.surface !== ZLinkDispatchErrorSurface.Channel
      && event.surface !== ZLinkDispatchErrorSurface.RouteMeshChannel)
  ) return undefined;
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

function dispatchErrorInfo(event: ZLinkRuntimeDispatchFailure): { readonly errorType?: string; readonly errorMessage?: string } {
  if (event.errorType !== undefined || event.errorMessage !== undefined) {
    return { errorType: event.errorType, errorMessage: event.errorMessage };
  }
  if (event.error === undefined) {
    return {};
  }
  if (event.error instanceof Error) {
    return { errorType: event.error.name, errorMessage: event.error.message };
  }
  return { errorType: typeof event.error, errorMessage: String(event.error) };
}
