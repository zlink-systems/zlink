import { RequestRuntime } from '../foundation/request-runtime';
import type { PendingOperation } from '../foundation/operation-registry';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  internalFrameworkErrorKind,
  requestResultToPublicErrorKind
} from '../framework-errors-internal';
import { waitRequestReply, remainingRequestTimeout } from '../messaging/request-deadline';
import { isZLinkBackendResultError } from '../backend/runtime-values';
import type { Message } from '../../contracts/Common/Message';
import {
  ZLinkSubmitStatus,
  classifySubmitResult,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import {
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  type ZLinkRuntimeMessageFlowResult,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import { throwIfAborted } from '../abort';
import {
  closeMessages,
  decodeChannelReply,
  encodeChannelEnvelopeParts,
  encodeChannelEnvelopePartsAtDeadline,
  newChannelCorrelationId,
  type ZLinkChannelEnvelopeCodecRegistry,
  ZLinkChannelMessageKind
} from './channel-envelope';
import { requirePublicFanoutTopic } from './fanout-service-wire';
import { ZLinkChannelDispatchServices } from './channel-dispatch-services';
import { codecsForFrameworkPacket } from './channel-framework-packets';
import { ZLinkChannelSocketRegistry } from './channel-socket-registry';
import { ZLinkFrameworkErrorKind, ZLinkFrameworkException } from '../../contracts';
import { ZLinkRouteDisconnectedError } from './route-disconnected-error';
import { runWithOutboundFlow } from '../diagnostics/flow-context';

const EMPTY_OUTBOUND_METADATA: ReadonlyMap<string, string> = new Map();

export class ZLinkChannelOutboundOperations {
  private readonly pendingRequests = new Map<string, number>();
  constructor(
    private readonly sockets: ZLinkChannelSocketRegistry,
    private readonly codecs: ZLinkChannelEnvelopeCodecRegistry,
    private readonly dispatchServices: ZLinkChannelDispatchServices,
    private readonly requests = new RequestRuntime()
  ) {}

  pendingRequestCount(channelName: string): number {
    return this.pendingRequests.get(channelName) ?? 0;
  }

  send(
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
  ): Promise<ZLinkSubmitResult> {
    // Call-scoped flow (spec 27 §4): the envelope encoder and the trace
    // points below share one ambient flow that does not outlive this call.
    return runWithOutboundFlow(this.dispatchServices.flowCreationEnabled(), () =>
      this.sendScoped(channelName, packetName, message, metadata)
    );
  }

  private async sendScoped(
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
  ): Promise<ZLinkSubmitResult> {
    const dealer = await this.sockets.awaitClientDealerForOutbound(channelName);
    if (dealer === undefined) {
      return { status: ZLinkSubmitStatus.Shutdown };
    }
    const parts = encodeChannelEnvelopeParts(
      ZLinkChannelMessageKind.Command,
      channelName,
      packetName,
      message,
      undefined,
      undefined,
      this.codecs,
      undefined,
      this.dispatchServices.flowCreationEnabled(),
      metadata
    ) as readonly Message[];
    try {
      await dealer.send(parts);
    } catch (error) {
      closeMessages(parts);
      if (isZLinkBackendResultError(error) && error.operation === 'submit') {
        return classifySubmitResult(error.result, 'One-way send', error.phase);
      }
      throw error;
    }
    this.traceSend(
      channelName,
      packetName,
      this.sockets.selectedClientServerRid(channelName, dealer)
    );
    return { status: ZLinkSubmitStatus.Submitted };
  }

  private traceSend(
    channelName: string,
    packetName: string | undefined,
    serverRid: string | undefined
  ): void {
    this.dispatchServices.beginOutbound(ZLinkMessageFlowOutcome.Sent)?.trace({
      surface: ZLinkDispatchErrorSurface.Channel,
      messageKind: ZLinkDispatchMessageKind.Send,
      channelName,
      channelRouteKind: 'client_server',
      serverRid,
      packetName,
      correlationId: undefined
    });
  }

  request<TReply>(
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    onAdmitted?: () => void,
    requestOperation?: PendingOperation<unknown>
  ): Promise<TReply> {
    return runWithOutboundFlow(this.dispatchServices.flowCreationEnabled(), () =>
      this.requestScoped<TReply>(
        channelName,
        packetName,
        request,
        timeoutMs,
        signal,
        metadata,
        onAdmitted,
        requestOperation
      )
    );
  }

  private requestScoped<TReply>(
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>
  ): Promise<TReply> {
    let correlationId: string | undefined;
    let selectedServerRid: string | undefined;
    const traceTerminal = (result: ZLinkRuntimeMessageFlowResult): void => {
      this.dispatchServices.beginOutbound(ZLinkMessageFlowOutcome.ReplyReceived, result)?.trace({
        surface: ZLinkDispatchErrorSurface.Channel,
        messageKind: ZLinkDispatchMessageKind.Request,
        channelName,
        channelRouteKind: 'client_server',
        serverRid: selectedServerRid,
        packetName,
        correlationId,
        result
      });
    };
    try {
      signal?.throwIfAborted();
      const operation = this.requests.begin(timeoutMs, signal, existing);
      const reply = this.requests.run<TReply>(
        operation,
        (receiver) => {
          this.measureRequest(channelName, operation);
          operation.mapCaller(
            (value) => {
              traceTerminal('succeeded');
              return value;
            },
            (error) => {
              if (!isRemoteChannelErrorReply(error))
                traceTerminal(requestTerminalResult(error, signal));
              throw error;
            }
          );

          void (async () => {
            const requestOperation = operation;
            const deadlineMs = requestOperation.deadlineMs;
            const deadlineUnixMs = timeoutMs === undefined ? undefined : Date.now() + timeoutMs;
            const readyWait = new AbortController();
            const readySignal =
              signal === undefined ? readyWait.signal : AbortSignal.any([signal, readyWait.signal]);
            const dealer = await waitRequestReply(
              this.sockets.awaitClientDealerForOutbound(channelName, readySignal),
              `Channel request '${channelName}'`,
              requestOperation,
              () => readyWait.abort()
            );
            if (dealer === undefined) {
              throw createInternalFrameworkException(
                ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
                `Channel '${channelName}' has no ready ClientServer server before its admission deadline.`
              );
            }
            const serverRid = this.sockets.selectedClientServerRid(channelName, dealer);
            selectedServerRid = serverRid;
            correlationId = newChannelCorrelationId();
            const parts = encodeChannelEnvelopePartsAtDeadline(
              ZLinkChannelMessageKind.Request,
              channelName,
              packetName,
              request,
              deadlineUnixMs,
              undefined,
              this.codecs,
              correlationId,
              this.dispatchServices.flowCreationEnabled(),
              metadata
            ) as readonly Message[];
            this.dispatchServices.beginOutbound(ZLinkMessageFlowOutcome.Sent)?.trace({
              surface: ZLinkDispatchErrorSurface.Channel,
              messageKind: ZLinkDispatchMessageKind.Request,
              channelName,
              channelRouteKind: 'client_server',
              serverRid,
              packetName,
              correlationId
            });
            dealer.request(
              parts,
              remainingRequestTimeout(`Channel request '${channelName}'`, deadlineMs),
              onAdmitted,
              requestOperation,
              receiver
            );
          })().catch((error) => receiver.operationFailed(operation.id, error, undefined));
        },
        (raw) => {
          const replyParts = raw as readonly Message[];
          try {
            return decodeChannelReply<TReply>(replyParts, this.codecs);
          } finally {
            closeMessages(replyParts);
          }
        },
        (error) => {
          if (
            signal?.aborted !== true &&
            isZLinkBackendResultError(error) &&
            error.operation === 'request'
          ) {
            return new ZLinkFrameworkException(
              requestResultToPublicErrorKind(error.result),
              `Channel '${channelName}' request failed with result ${error.result}.`,
              error
            );
          }
          return error;
        }
      );
      return reply;
    } catch (error) {
      // A framework peer has already made the terminal dispatch error record
      // before it sends an Error reply.  Do not turn that same remote dispatch
      // failure into a second local `reply_received` terminal at Errors level.
      if (!isRemoteChannelErrorReply(error)) traceTerminal(requestTerminalResult(error, signal));
      return Promise.reject(error);
    }
  }

  tryPublish(
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: unknown,
    metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
  ): ZLinkSubmitResult {
    return runWithOutboundFlow(this.dispatchServices.flowCreationEnabled(), () =>
      this.tryPublishScoped(channelName, topic, packetName, event, metadata)
    );
  }

  private tryPublishScoped(
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: unknown,
    metadata: ReadonlyMap<string, string>
  ): ZLinkSubmitResult {
    requirePublicFanoutTopic(topic);
    const publisher = this.sockets['publisher'](channelName);
    const parts = encodeChannelEnvelopeParts(
      ZLinkChannelMessageKind.Publish,
      channelName,
      packetName,
      event,
      undefined,
      topic,
      this.codecs,
      undefined,
      // Spec 27 §5: every Classic-fanout branch carries the origin flow pair
      // while tracing is enabled; Off suppresses envelope flow fields (§4).
      this.dispatchServices.flowCreationEnabled(),
      metadata
    ) as readonly Message[];
    publisher.publish(topic, parts);
    return publishResult(ZLinkSubmitStatus.Submitted);
  }

  publish(
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: unknown,
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
  ): Promise<ZLinkSubmitResult> {
    return runWithOutboundFlow(this.dispatchServices.flowCreationEnabled(), () =>
      this.publishScoped(channelName, topic, packetName, event, signal, metadata)
    );
  }

  private async publishScoped(
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: unknown,
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
  ): Promise<ZLinkSubmitResult> {
    throwIfAborted(signal);
    requirePublicFanoutTopic(topic);
    const publisher = this.sockets['publisher'](channelName);
    const parts = encodeChannelEnvelopeParts(
      ZLinkChannelMessageKind.Publish,
      channelName,
      packetName,
      event,
      undefined,
      topic,
      this.codecs,
      undefined,
      // Spec 27 §5: every Classic-fanout branch carries the origin flow pair
      // while tracing is enabled; Off suppresses envelope flow fields (§4).
      this.dispatchServices.flowCreationEnabled(),
      metadata
    ) as readonly Message[];
    throwIfAborted(signal);
    publisher.publish(topic, parts);
    return publishResult(ZLinkSubmitStatus.Submitted);
  }

  routeSubmit(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
  ): Promise<ZLinkSubmitResult> {
    return runWithOutboundFlow(this.dispatchServices.flowCreationEnabled(), () =>
      this.routeSubmitScoped(routerChannelId, targetNodeRid, packetName, message, metadata)
    );
  }

  private async routeSubmitScoped(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
  ): Promise<ZLinkSubmitResult> {
    const router = this.sockets.routeRouter(routerChannelId);
    const parts = encodeChannelEnvelopeParts(
      ZLinkChannelMessageKind.Command,
      routerChannelId,
      packetName,
      message,
      undefined,
      undefined,
      codecsForFrameworkPacket(packetName, this.codecs),
      undefined,
      this.dispatchServices.flowCreationEnabled(),
      metadata
    ) as readonly Message[];
    try {
      await router.send(targetNodeRid, parts);
    } catch (error) {
      if (isZLinkBackendResultError(error) && error.operation === 'submit') {
        return classifySubmitResult(error.result, 'One-way send', error.phase);
      }
      throw error;
    }
    this.traceRouteSend(routerChannelId, targetNodeRid, packetName);
    return { status: ZLinkSubmitStatus.Submitted };
  }

  private traceRouteSend(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined
  ): void {
    this.dispatchServices.beginOutbound(ZLinkMessageFlowOutcome.Sent)?.trace({
      surface: ZLinkDispatchErrorSurface.RouteMeshChannel,
      messageKind: ZLinkDispatchMessageKind.Send,
      channelName: routerChannelId,
      packetName,
      correlationId: undefined,
      targetRid: targetNodeRid
    });
  }

  routeRequest<TReply>(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    onAdmitted?: () => void,
    requestOperation?: PendingOperation<unknown>
  ): Promise<TReply> {
    return runWithOutboundFlow(this.dispatchServices.flowCreationEnabled(), () =>
      this.routeRequestScoped<TReply>(
        routerChannelId,
        targetNodeRid,
        packetName,
        request,
        timeoutMs,
        signal,
        metadata,
        onAdmitted,
        requestOperation
      )
    );
  }

  private routeRequestScoped<TReply>(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    onAdmitted?: () => void,
    existing?: PendingOperation<unknown>
  ): Promise<TReply> {
    let correlationId: string | undefined;
    const traceTerminal = (result: ZLinkRuntimeMessageFlowResult): void => {
      this.dispatchServices.beginOutbound(ZLinkMessageFlowOutcome.ReplyReceived, result)?.trace({
        surface: ZLinkDispatchErrorSurface.RouteMeshChannel,
        messageKind: ZLinkDispatchMessageKind.Request,
        channelName: routerChannelId,
        channelRouteKind: 'route_mesh',
        packetName,
        correlationId,
        targetRid: targetNodeRid,
        result
      });
    };
    try {
      signal?.throwIfAborted();
      const operation = this.requests.begin(timeoutMs, signal, existing);
      const reply = this.requests.run<TReply>(
        operation,
        (receiver) => {
          this.measureRequest(routerChannelId, operation);
          operation.mapCaller(
            (value) => {
              traceTerminal('succeeded');
              return value;
            },
            (error) => {
              if (!isRemoteChannelErrorReply(error))
                traceTerminal(requestTerminalResult(error, signal));
              throw error;
            }
          );

          const requestOperation = operation;
          const router = this.sockets.routeRouter(routerChannelId);
          if (this.sockets.routeMemberStatus(routerChannelId, targetNodeRid) === 'missing') {
            throw createInternalFrameworkException(
              ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
              `Route channel '${routerChannelId}' has no member '${targetNodeRid}'.`
            );
          }
          correlationId = newChannelCorrelationId();
          const parts = encodeChannelEnvelopeParts(
            ZLinkChannelMessageKind.Request,
            routerChannelId,
            packetName,
            request,
            timeoutMs,
            undefined,
            codecsForFrameworkPacket(packetName, this.codecs),
            correlationId,
            this.dispatchServices.flowCreationEnabled(),
            metadata
          ) as readonly Message[];
          this.dispatchServices.beginOutbound(ZLinkMessageFlowOutcome.Sent)?.trace({
            surface: ZLinkDispatchErrorSurface.RouteMeshChannel,
            messageKind: ZLinkDispatchMessageKind.Request,
            channelName: routerChannelId,
            channelRouteKind: 'route_mesh',
            packetName,
            correlationId,
            targetRid: targetNodeRid
          });
          router.request(targetNodeRid, parts, timeoutMs, onAdmitted, requestOperation, receiver);
        },
        (raw) => {
          const replyParts = raw as readonly Message[];
          try {
            return decodeChannelReply<TReply>(replyParts, this.codecs);
          } finally {
            closeMessages(replyParts);
          }
        },
        (error) => {
          if (
            this.sockets.routeMemberStatus(routerChannelId, targetNodeRid) === 'disconnected' &&
            (isSubmitDeadline(error) || error instanceof ZLinkRouteDisconnectedError)
          ) {
            return createInternalFrameworkException(
              ZLinkFrameworkInternalErrorKind.RouteNotConnected,
              `Route channel '${routerChannelId}' member '${targetNodeRid}' is not connected.`,
              true,
              error
            );
          }
          return error;
        }
      );
      return reply;
    } catch (error) {
      if (!isRemoteChannelErrorReply(error)) traceTerminal(requestTerminalResult(error, signal));
      return Promise.reject(error);
    }
  }

  private measureRequest(channel: string, operation: PendingOperation<unknown>): void {
    this.pendingRequests.set(channel, this.pendingRequestCount(channel) + 1);
    const complete = (): void => {
      const remaining = this.pendingRequestCount(channel) - 1;
      if (remaining === 0) this.pendingRequests.delete(channel);
      else this.pendingRequests.set(channel, remaining);
    };
    operation.mapCaller(
      (value) => {
        complete();
        return value;
      },
      (error) => {
        complete();
        throw error;
      }
    );
  }
}

function isSubmitDeadline(error: unknown): boolean {
  return (
    error instanceof ZLinkFrameworkException &&
    internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.DeadlineExceeded
  );
}

function publishResult(status: ZLinkSubmitStatus): ZLinkSubmitResult {
  return { status };
}

export function requestTerminalResult(
  error: unknown,
  signal: AbortSignal | undefined
): ZLinkRuntimeMessageFlowResult {
  if (signal?.aborted === true) return 'cancelled';
  if (error instanceof ZLinkFrameworkException) {
    if (error.kind === ZLinkFrameworkErrorKind.ShuttingDown) return 'shutdown';
  }
  return 'failed';
}

export function isRemoteChannelErrorReply(error: unknown): boolean {
  return (
    error instanceof ZLinkFrameworkException &&
    'origin' in error &&
    (error as { readonly origin?: unknown }).origin !== undefined
  );
}
