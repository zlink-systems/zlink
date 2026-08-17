import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException, internalFrameworkErrorKind  } from '../framework-errors-internal';
import type {
  ZLinkMessageContext,
  ZLinkHandlerFilter,
  ZLinkHandlerFilterContext,
  ZLinkUnhandledDispatchOptions,
  ZLinkFlowOrigin
} from '../../contracts';
import {
  ZLinkFrameworkErrorKind,
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
  ZLinkChannelMessageKind,
  type ZLinkChannelEnvelope,
  type ZLinkChannelEnvelopeCodecRegistry
} from './channel-envelope';
import type { ZLinkMalformedChannelHeaderInfo } from './channel-envelope-inspection';
import type { ZLinkDispatchErrorReporter } from './dispatch-error-reporter';
import { createInboundFlow, runWithFlow } from '../diagnostics/flow-context';
import { releaseApplicationJobPermitBeforeHandler } from '../application-jobs/application-job-queue-scope';

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
    this.trace(ZLinkMessageFlowOutcome.Admitted, dispatch.fields);
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
      const invocation = await runWithFlow(flow, () => this.invoke(
          dispatch.envelope,
          dispatch.codecs,
          dispatch.handler!,
          dispatch.context,
          dispatch.fields,
          dispatch.signal
        ));
      if (invocation.handlerInvoked) {
        this.trace(ZLinkMessageFlowOutcome.Completed, dispatch.fields);
      }
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
    this.trace(ZLinkMessageFlowOutcome.Admitted, dispatch.fields);
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
        // Spec 26 §3.1: a failed reply write means the reply delivery path is
        // gone; `unexpected_reply` names a surplus/late reply instead.
        this.report(
          dispatch.fields,
          ZLinkDispatchErrorReason.ReplyPathMissing,
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
      // Spec 26 §3.1: a failed reply write means the reply delivery path is
      // gone; `unexpected_reply` names a surplus/late reply instead.
      this.report(
        dispatch.fields,
        ZLinkDispatchErrorReason.ReplyPathMissing,
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

  /**
   * Terminal handling for an envelope that failed structural decoding.
   * Requests receive a protocol error reply plus a
   * `zlink.dispatch_error(invalid_frame/reply_error)` record; one-way frames
   * are dropped with `invalid_frame/drop`. Identifiers are used only when
   * they could be read from the invalid frame (spec 27 §7 — never fabricated).
   */
  async dispatchMalformed(input: {
    readonly info: ZLinkMalformedChannelHeaderInfo;
    readonly error: unknown;
    /** The transport exposed a reply path (e.g. a request sequence). */
    readonly transportRequest: boolean;
    readonly sourceRid?: string;
    readonly writeProtocolError?: (error: ZLinkFrameworkException) => Promise<void>;
  }): Promise<void> {
    const { info } = input;
    const isRequest = info.kind === ZLinkChannelMessageKind.Request
      || (info.kind === undefined && input.transportRequest);
    let replied = false;
    if (
      isRequest
      && input.writeProtocolError !== undefined
      && info.correlationId !== undefined
    ) {
      const protocolError = new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.ProtocolError,
        `Channel '${this.options.channelName}' received a malformed envelope.`
      );
      try {
        await input.writeProtocolError(protocolError);
        replied = true;
      } catch {
        // The reply path failed; the record below downgrades to a drop.
      }
    }
    this.options.dispatchErrors.report({
      surface: this.options.surface,
      messageKind: isRequest ? ZLinkDispatchMessageKind.Request : ZLinkDispatchMessageKind.Send,
      reason: ZLinkDispatchErrorReason.InvalidFrame,
      action: replied ? ZLinkDispatchErrorAction.ReplyError : ZLinkDispatchErrorAction.Drop,
      packetName: info.messageName,
      channelName: this.options.channelName,
      sourceRid: input.sourceRid,
      correlationId: info.correlationId,
      flowId: info.flowId,
      flowOrigin: info.flowOrigin,
      error: input.error
    });
  }

  private async invoke<TContext extends ZLinkMessageContext, TResult>(
    envelope: ZLinkChannelEnvelope,
    codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
    handler: ZLinkChannelDispatchHandler<TContext, TResult>,
    context: TContext,
    fields: ZLinkChannelDispatchFields,
    signal?: AbortSignal
  ): Promise<{ readonly handlerInvoked: boolean; readonly value?: TResult }> {
    releaseApplicationJobPermitBeforeHandler();
    if (this.filters.length === 0) {
      //  Fast path for the dominant unfiltered dispatch: skip the filter
      //  context object and chain machinery entirely.
      const payload = decodeChannelPayload(envelope, codecs);
      this.trace(ZLinkMessageFlowOutcome.Dispatched, fields);
      return { handlerInvoked: true, value: await handler.handle(payload, context) };
    }
    let value: TResult | undefined;
    const handlerInvoked = await invokeZLinkHandlerFilters(
      this.filters,
      this.createFilterContext(context, fields),
      async () => {
        // Filters are the channel admission gate. Decode only after they have
        // allowed the handler turn to proceed.
        const payload = decodeChannelPayload(envelope, codecs);
        this.trace(ZLinkMessageFlowOutcome.Dispatched, fields);
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
    const classicFanout = fields.messageKind === ZLinkDispatchMessageKind.Publish;
    this.options.dispatchErrors.report({
      surface: classicFanout
        ? ZLinkDispatchErrorSurface.ClassicFanout
        : this.options.surface,
      messageKind: classicFanout
        ? ZLinkDispatchMessageKind.Send
        : fields.messageKind,
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
    if (fields.messageKind === ZLinkDispatchMessageKind.Publish) return;
    const flow = this.options.dispatchErrors.flow;
    const tracePoint = flow.begin(outcome, undefined, fields.flowId);
    if (tracePoint === undefined) return;
    tracePoint.trace({
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
