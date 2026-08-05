import type {
  Type,
  ZLinkHandlerFilter,
  ZLinkHandlerFilterContext,
  ZLinkMessageContext,
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type {
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type {
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { ZLinkFrameworkRegistration } from '../configuration';
import {
  ZLinkMessageFlowTracer,
  ZLinkRuntimeMetrics,
  createDiagnosticsContext,
  createMessageFlowModeCell,
  flowIfEnabled,
  type ZLinkDiagnosticsContext,
  type ZLinkMessageFlowModeCell
} from '../diagnostics';
import type { ZLinkRuntimeTaskErrorSink } from '../execution';
import { ZLinkDispatchErrorReporter } from './dispatch-error-reporter';
import { invokeZLinkHandlerFilters } from '../handlers';
import { runInHandlerInstanceScope } from '../handlers/handler-instance-scope';
import { handlerFilterScope } from './handler-filter-scope';
import type {
  ZLinkChannelRequestHandler,
  ZLinkChannelSendHandler,
  ZLinkRouteRuntimeRequestHandler,
  ZLinkRouteRuntimeSendHandler
} from './channel-dispatchers';

export interface ZLinkChannelOutboundTrace {
  readonly surface: ZLinkDispatchErrorSurface;
  readonly messageKind: ZLinkDispatchMessageKind;
  readonly channelName: string;
  readonly packetName: string | undefined;
  readonly correlationId: string | undefined;
  readonly topic?: string;
  readonly sourceRid?: string;
}

export class ZLinkChannelDispatchServices {
  private readonly dispatchErrorReporters = new WeakMap<ZLinkRuntimeTaskErrorSink, ZLinkDispatchErrorReporter>();
  private messageFlowModeCellValue?: ZLinkMessageFlowModeCell;
  private diagnosticsContextValue?: ZLinkDiagnosticsContext;
  private handlerFiltersValue?: readonly ZLinkHandlerFilter[];
  private outboundFlowValue?: ZLinkMessageFlowTracer;
  private readonly metricsValue: ZLinkRuntimeMetrics;

  constructor(
    private readonly registration: ZLinkFrameworkRegistration,
    private readonly providerResolver?: ZLinkProviderResolver,
    private readonly configuredMessageFlowModeCell?: ZLinkMessageFlowModeCell
  ) {
    this.metricsValue = new ZLinkRuntimeMetrics(registration.metrics?.meterProvider);
  }

  metrics(): ZLinkRuntimeMetrics {
    return this.metricsValue;
  }

  flowCreationEnabled(): boolean {
    return this.outboundFlow().flowCreationEnabled();
  }

  dispatchErrorReporter(errorSink: ZLinkRuntimeTaskErrorSink): ZLinkDispatchErrorReporter {
    const existing = this.dispatchErrorReporters.get(errorSink);
    if (existing !== undefined) {
      return existing;
    }
    const reporter = new ZLinkDispatchErrorReporter(
      undefined,
      undefined,
      errorSink,
      this.diagnosticsContext(),
      this.metricsValue
    );
    this.dispatchErrorReporters.set(errorSink, reporter);
    return reporter;
  }

  handlerFilters(): readonly ZLinkHandlerFilter[] {
    if (this.handlerFiltersValue !== undefined) {
      return this.handlerFiltersValue;
    }
    this.handlerFiltersValue = this.registration.filterTypes.length === 0
      ? []
      : [{ invoke: (context, next, signal) => this.invokeHandlerFilters(context, next, signal) }];
    return this.handlerFiltersValue;
  }

  channelRequestHandler(handlerType: Type): ZLinkChannelRequestHandler {
    return {
      handle: (payload, context) => this.withHandlerScope(context, async (scope) => {
        const handler = await scope.resolve(
          handlerType as Type<ZLinkChannelRequestHandler>
        );
        return handler.handle(payload, context);
      })
    };
  }

  channelSendHandler(handlerType: Type): ZLinkChannelSendHandler {
    return {
      handle: (payload, context) => this.withHandlerScope(context, async (scope) => {
        const handler = await scope.resolve(
          handlerType as Type<ZLinkChannelSendHandler>
        );
        await handler.handle(payload, context);
      })
    };
  }

  routeRequestHandler(handlerType: Type): ZLinkRouteRuntimeRequestHandler {
    return {
      handle: (payload, context) => this.withHandlerScope(context, async (scope) => {
        const handler = await scope.resolve(
          handlerType as Type<ZLinkRouteRuntimeRequestHandler>
        );
        return handler.handle(payload, context);
      })
    };
  }

  routeSendHandler(handlerType: Type): ZLinkRouteRuntimeSendHandler {
    return {
      handle: (payload, context) => this.withHandlerScope(context, async (scope) => {
        const handler = await scope.resolve(
          handlerType as Type<ZLinkRouteRuntimeSendHandler>
        );
        await handler.handle(payload, context);
      })
    };
  }

  private async invokeHandlerFilters(
    context: ZLinkHandlerFilterContext,
    next: () => Promise<void>,
    signal?: AbortSignal
  ): Promise<void> {
    const scoped = handlerFilterScope(this.providerResolver);
    if (scoped !== undefined) {
      await scoped(context, async (resolver) => {
        await invokeZLinkHandlerFilters(
          await Promise.all(
            this.registration.filterTypes.map((filterType) => resolver.resolve(filterType))
          ),
          context,
          next,
          signal
        );
      });
      return;
    }
    await this.withHandlerScope(context, async (scope) => {
      await invokeZLinkHandlerFilters(
        await Promise.all(this.registration.filterTypes.map((filterType) => scope.resolve(filterType))),
        context,
        next,
        signal
      );
    });
  }

  private withHandlerScope<T>(
    context: ZLinkMessageContext,
    callback: Parameters<typeof runInHandlerInstanceScope<T>>[2]
  ): Promise<T> {
    return runInHandlerInstanceScope(this.providerResolver, context, callback);
  }

  traceOutbound(
    outcome: ZLinkMessageFlowOutcome,
    createTrace: () => ZLinkChannelOutboundTrace
  ): void {
    const flow = flowIfEnabled(this.outboundFlow(), outcome);
    if (flow === undefined) {
      return;
    }
    const trace = createTrace();
    flow.trace({
      outcome,
      surface: trace.surface,
      messageKind: trace.messageKind,
      channelName: trace.channelName,
      packetName: trace.packetName,
      correlationId: trace.correlationId,
      topic: trace.topic,
      sourceRid: trace.sourceRid
    });
  }

  private messageFlowModeCell(): ZLinkMessageFlowModeCell {
    this.messageFlowModeCellValue ??=
      this.configuredMessageFlowModeCell ?? createMessageFlowModeCell(this.registration.dispatch);
    return this.messageFlowModeCellValue;
  }

  private diagnosticsContext(): ZLinkDiagnosticsContext {
    this.diagnosticsContextValue ??= createDiagnosticsContext(
      this.registration.dispatch,
      this.providerResolver,
      this.messageFlowModeCell()
    );
    return this.diagnosticsContextValue;
  }

  private outboundFlow(): ZLinkMessageFlowTracer {
    this.outboundFlowValue ??= new ZLinkMessageFlowTracer(
      this.diagnosticsContext(),
      { reportRuntimeTaskException() {} },
      this.metricsValue
    );
    return this.outboundFlowValue;
  }
}
