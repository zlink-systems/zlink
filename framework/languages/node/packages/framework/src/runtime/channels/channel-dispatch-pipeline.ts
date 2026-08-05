import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException, internalFrameworkErrorKind  } from '../framework-errors-internal';
import type {
  ZLinkMessageContext,
  ZLinkHandlerFilter,
  ZLinkHandlerFilterContext,
  ZLinkUnhandledDispatchOptions,
  ZLinkFlowOrigin
} from '../../contracts';
import {
  ZLinkFrameworkException,
  ZLinkHandlerDispatchKind,
  ZLinkUnhandledDispatchAction
} from '../../contracts';
import {
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchMessageKind,
  ZLinkDispatchErrorSurface
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import { invokeZLinkHandlerFilters } from '../handlers';
import {
  decodeChannelPayload,
  type ZLinkChannelEnvelope,
  type ZLinkChannelEnvelopeCodecRegistry
} from './channel-envelope';
import type { ZLinkDispatchErrorReporter } from './dispatch-error-reporter';
import { createInboundFlow, runWithFlow } from '../diagnostics/flow-context';

export interface ZLinkChannelDispatchPipelineOptions {
  readonly dispatchErrors: ZLinkDispatchErrorReporter;
  readonly surface: ZLinkDispatchErrorSurface;
  readonly channelName: string;
  readonly filters?: readonly ZLinkHandlerFilter[];
  readonly unhandled?: ZLinkUnhandledDispatchOptions;
}

export interface ZLinkChannelDispatchFields {
  readonly messageKind: ZLinkDispatchMessageKind;
  readonly packetName: string;
  readonly correlationId?: string;
  readonly topic?: string;
  readonly sourceRid?: string;
  readonly flowId?: string;
  readonly flowOrigin?: ZLinkFlowOrigin;
}

interface ZLinkChannelDispatchHandler<TContext extends ZLinkMessageContext, TResult> {
  handle(payload: unknown, context: TContext): Promise<TResult> | TResult;
}

interface ZLinkOneWayDispatch<TContext extends ZLinkMessageContext> {
  readonly fields: ZLinkChannelDispatchFields;
  readonly envelope: ZLinkChannelEnvelope;
  readonly codecs?: ZLinkChannelEnvelopeCodecRegistry;
  readonly handler?: ZLinkChannelDispatchHandler<TContext, void>;
  readonly context: TContext;
  readonly signal?: AbortSignal;
}

interface ZLinkRequestDispatch<TContext extends ZLinkMessageContext> {
  readonly fields: ZLinkChannelDispatchFields;
  readonly envelope: ZLinkChannelEnvelope;
  readonly codecs?: ZLinkChannelEnvelopeCodecRegistry;
  readonly handler?: ZLinkChannelDispatchHandler<TContext, unknown>;
  readonly context: TContext;
  readonly signal?: AbortSignal;
  readonly missingHandlerMessage: string;
  readonly writeReply: (reply: unknown) => Promise<void>;
  readonly writeError: (error: unknown) => Promise<void>;
}

export class ZLinkChannelDispatchPipeline {
  private readonly filters: readonly ZLinkHandlerFilter[];
  private readonly unhandled: ZLinkUnhandledDispatchOptions;

  constructor(private readonly options: ZLinkChannelDispatchPipelineOptions) {
    this.filters = options.filters ?? [];
    this.unhandled = options.unhandled ?? DEFAULT_UNHANDLED_DISPATCH;
  }

  async dispatchOneWay<TContext extends ZLinkMessageContext>(dispatch: ZLinkOneWayDispatch<TContext>): Promise<void> {
    this.trace(ZLinkMessageFlowOutcome.Received, dispatch.fields);
    if (dispatch.handler === undefined) {
      const action = dispatch.fields.messageKind === ZLinkDispatchMessageKind.Publish
        ? this.unhandled.publish
        : this.unhandled.send;
      this.report(
        dispatch.fields,
        ZLinkDispatchErrorReason.HandlerMissing,
        ZLinkDispatchErrorAction.Drop
      );
      this.applyOneWayUnhandled(action, dispatch.fields);
      return;
    }

    try {
      const flow = createInboundFlow(
        dispatch.fields.flowId,
        dispatch.fields.flowOrigin,
        this.options.dispatchErrors.flow.flowCreationEnabled()
      );
      await runWithFlow(flow, () => this.invoke(
          dispatch.envelope,
          dispatch.codecs,
          dispatch.handler!,
          dispatch.context,
          dispatch.fields,
          dispatch.signal
        ));
      this.trace(ZLinkMessageFlowOutcome.Dispatched, dispatch.fields);
    } catch (error) {
      this.report(
        dispatch.fields,
        this.failureReason(error),
        ZLinkDispatchErrorAction.Drop,
        error
      );
    }
  }

  async dispatchRequest<TContext extends ZLinkMessageContext>(dispatch: ZLinkRequestDispatch<TContext>): Promise<void> {
    this.trace(ZLinkMessageFlowOutcome.Received, dispatch.fields);
    if (dispatch.handler === undefined) {
      const missing = createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.HandlerNotFound,
        dispatch.missingHandlerMessage
      );
      if (this.unhandled.request === ZLinkUnhandledDispatchAction.ReplyError) {
        await dispatch.writeError(missing);
      }
      this.report(
        dispatch.fields,
        ZLinkDispatchErrorReason.HandlerMissing,
        this.unhandled.request === ZLinkUnhandledDispatchAction.ReplyError
          ? ZLinkDispatchErrorAction.ReplyError
          : ZLinkDispatchErrorAction.Drop
      );
      if (this.unhandled.request === ZLinkUnhandledDispatchAction.Throw) {
        throw missing;
      }
      return;
    }

    let reply: unknown;
    try {
      const flow = createInboundFlow(
        dispatch.fields.flowId,
        dispatch.fields.flowOrigin,
        this.options.dispatchErrors.flow.flowCreationEnabled()
      );
      const invocation = await runWithFlow(flow, () => this.invoke(
          dispatch.envelope,
          dispatch.codecs,
          dispatch.handler!,
          dispatch.context,
          dispatch.fields,
          dispatch.signal
        ));
      if (!invocation.handlerInvoked) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RequestRejected,
          `A handler filter rejected '${this.options.channelName}:${dispatch.fields.packetName}'.`
        );
      }
      reply = invocation.value;
    } catch (error) {
      try {
        await dispatch.writeError(error);
      } catch (replyError) {
        this.report(
          dispatch.fields,
          ZLinkDispatchErrorReason.UnexpectedReply,
          ZLinkDispatchErrorAction.Drop,
          replyError
        );
        return;
      }
      this.report(
        dispatch.fields,
        this.failureReason(error),
        ZLinkDispatchErrorAction.ReplyError,
        error
      );
      return;
    }

    try {
      await dispatch.writeReply(reply);
      this.trace(ZLinkMessageFlowOutcome.Replied, dispatch.fields);
    } catch (error) {
      this.report(
        dispatch.fields,
        ZLinkDispatchErrorReason.UnexpectedReply,
        ZLinkDispatchErrorAction.Drop,
        error
      );
    }
  }

  dropMissingReplyPath(fields: ZLinkChannelDispatchFields): void {
    this.report(
      fields,
      ZLinkDispatchErrorReason.ReplyPathMissing,
      ZLinkDispatchErrorAction.Drop
    );
  }

  private async invoke<TContext extends ZLinkMessageContext, TResult>(
    envelope: ZLinkChannelEnvelope,
    codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
    handler: ZLinkChannelDispatchHandler<TContext, TResult>,
    context: TContext,
    fields: ZLinkChannelDispatchFields,
    signal?: AbortSignal
  ): Promise<{ readonly handlerInvoked: boolean; readonly value?: TResult }> {
    let value: TResult | undefined;
    const handlerInvoked = await invokeZLinkHandlerFilters(
      this.filters,
      this.createFilterContext(context, fields),
      async () => {
        // Filters are the channel admission gate. Decode only after they have
        // allowed the handler turn to proceed.
        const payload = decodeChannelPayload(envelope, codecs);
        value = await handler.handle(payload, context);
      },
      signal
    );
    return { handlerInvoked, value };
  }

  private createFilterContext(
    context: ZLinkMessageContext,
    fields: ZLinkChannelDispatchFields
  ): ZLinkHandlerFilterContext {
    const dispatchKind = this.dispatchKind(context, fields);
    return {
      meshName: dispatchKind === ZLinkHandlerDispatchKind.ClassicFanout
        ? undefined
        : context.meshName,
      channelName: context.channelName,
      packetName: context.packetName,
      contentType: context.contentType,
      metadata: context.metadata,
      correlationId: context.correlationId,
      dispatchKind
    };
  }

  private dispatchKind(
    context: ZLinkMessageContext,
    fields: ZLinkChannelDispatchFields
  ): ZLinkHandlerDispatchKind {
    if (fields.messageKind === ZLinkDispatchMessageKind.Publish) {
      return ZLinkHandlerDispatchKind.ClassicFanout;
    }
    const nodeDirect =
      this.options.surface === ZLinkDispatchErrorSurface.RouteMeshChannel
      && context.channelName === undefined;
    if (fields.messageKind === ZLinkDispatchMessageKind.Request) {
      return nodeDirect
        ? ZLinkHandlerDispatchKind.NodeDirectRequest
        : ZLinkHandlerDispatchKind.ChannelRequest;
    }
    return nodeDirect
      ? ZLinkHandlerDispatchKind.NodeDirectSend
      : ZLinkHandlerDispatchKind.ChannelSend;
  }

  private report(
    fields: ZLinkChannelDispatchFields,
    reason: ZLinkDispatchErrorReason,
    action: ZLinkDispatchErrorAction,
    error?: unknown
  ): void {
    this.options.dispatchErrors.report({
      surface: this.options.surface,
      messageKind: fields.messageKind,
      reason,
      action,
      packetName: fields.packetName,
      channelName: this.options.channelName,
      topic: fields.topic,
      sourceRid: fields.sourceRid,
      correlationId: fields.correlationId,
      flowId: fields.flowId,
      flowOrigin: fields.flowOrigin,
      error
    });
  }

  private trace(outcome: ZLinkMessageFlowOutcome, fields: ZLinkChannelDispatchFields): void {
    if (fields.messageKind === ZLinkDispatchMessageKind.Publish) {
      return;
    }
    const flow = this.options.dispatchErrors.flow;
    if (!flow.enabled(outcome)) {
      return;
    }
    flow.trace({
      outcome,
      surface: this.options.surface,
      messageKind: fields.messageKind,
      packetName: fields.packetName,
      channelName: this.options.channelName,
      topic: fields.topic,
      sourceRid: fields.sourceRid,
      correlationId: fields.correlationId,
      flowId: fields.flowId,
      flowOrigin: fields.flowOrigin
    });
  }

  private failureReason(error: unknown): ZLinkDispatchErrorReason {
    return error instanceof ZLinkFrameworkException
      && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed
      ? ZLinkDispatchErrorReason.PayloadDecodeFailed
      : ZLinkDispatchErrorReason.HandlerException;
  }

  private applyOneWayUnhandled(
    action: ZLinkUnhandledDispatchAction,
    fields: ZLinkChannelDispatchFields
  ): void {
    if (action === ZLinkUnhandledDispatchAction.Throw) {
      throw new Error(`No ${fields.messageKind} handler is registered for '${this.options.channelName}:${fields.packetName}'.`);
    }
  }
}

const DEFAULT_UNHANDLED_DISPATCH: ZLinkUnhandledDispatchOptions = {
  request: ZLinkUnhandledDispatchAction.ReplyError,
  send: ZLinkUnhandledDispatchAction.LogAndDrop,
  publish: ZLinkUnhandledDispatchAction.LogAndDrop
};
