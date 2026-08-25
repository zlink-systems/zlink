import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  internalFrameworkErrorKind,
  requestResultToPublicErrorKind
} from '../framework-errors-internal';
import { isZLinkBackendResultError } from '../backend/runtime-values';
import type { Message } from '../../contracts/Common/Message';
import {
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import {
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  type ZLinkRuntimeMessageFlowResult,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import { awaitWithAbort, throwIfAborted } from '../abort';
import {
  closeMessages,
  decodeChannelReply,
  encodeChannelEnvelopeParts,
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

const CHANNEL_REQUEST_LOOP_KEEPALIVE_MS = 0x7fff_ffff;

//  Shared default for the dominant no-metadata call; avoids a Map allocation
//  per outbound operation.
const EMPTY_OUTBOUND_METADATA: ReadonlyMap<string, string> = new Map();

export class ZLinkChannelOutboundOperations {
  private readonly pendingRequests = new Map<string, number>();
  constructor(
    private readonly sockets: ZLinkChannelSocketRegistry,
    private readonly codecs: ZLinkChannelEnvelopeCodecRegistry,
    private readonly dispatchServices: ZLinkChannelDispatchServices
  ) {}

  pendingRequestCount(channelName: string): number {
    return this.pendingRequests.get(channelName) ?? 0;
  }

  send(
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
  ): Promise<ZLinkSubmitResult> {
    // Call-scoped flow (spec 27 §4): the envelope encoder and the trace
    // points below share one ambient flow that does not outlive this call.
    return runWithOutboundFlow(
      this.dispatchServices.flowCreationEnabled(),
      () => this.sendScoped(channelName, packetName, message, signal, metadata)
    );
  }

  private async sendScoped(
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
  ): Promise<ZLinkSubmitResult> {
    throwIfAborted(signal);
    const dealer = await this.sockets.awaitClientDealerForOutbound(channelName, signal);
    if (dealer === undefined) {
      return {
        status: this.sockets.hasKnownClientServerTargets(channelName)
          ? ZLinkSubmitStatus.RouteNotConnected
          : ZLinkSubmitStatus.TargetNotFound
      };
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
      if (isSubmitDeadline(error)) {
        this.dispatchServices.beginOutbound(ZLinkMessageFlowOutcome.Backpressured)?.trace({
          surface: ZLinkDispatchErrorSurface.Channel,
          messageKind: ZLinkDispatchMessageKind.Send,
          channelName,
          channelRouteKind: 'client_server',
          packetName,
          correlationId: undefined,
          result: 'backpressured'
        });
        return { status: ZLinkSubmitStatus.TimedOut };
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
      correlationId: undefined,
    });
  }

  request<TReply>(
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply> {
    return keepChannelRequestAlive(runWithOutboundFlow(
      this.dispatchServices.flowCreationEnabled(),
      () => this.requestScoped<TReply>(channelName, packetName, request, timeoutMs, signal, metadata)
    ));
  }

  private async requestScoped<TReply>(
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply> {
    let correlationId: string | undefined;
    let selectedServerRid: string | undefined;
    let terminalRecorded = false;
    const traceTerminal = (result: ZLinkRuntimeMessageFlowResult): void => {
      if (terminalRecorded) return;
      terminalRecorded = true;
      this.dispatchServices.beginOutbound(
        ZLinkMessageFlowOutcome.ReplyReceived,
        result
      )?.trace({
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
      throwIfAborted(signal);
      const dealer = await this.sockets.awaitClientDealerForOutbound(channelName, signal);
      if (dealer === undefined) {
        throw createInternalFrameworkException(
          this.sockets.hasKnownClientServerTargets(channelName)
            ? ZLinkFrameworkInternalErrorKind.RouteNotConnected
            : ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
          this.sockets.hasKnownClientServerTargets(channelName)
            ? `Channel '${channelName}' has known ClientServer targets but no ready server.`
            : `Channel '${channelName}' has no ready ClientServer server.`
        );
      }
      const serverRid = this.sockets.selectedClientServerRid(channelName, dealer);
      selectedServerRid = serverRid;
      correlationId = newChannelCorrelationId();
      const parts = encodeChannelEnvelopeParts(
        ZLinkChannelMessageKind.Request,
        channelName,
        packetName,
        request,
        timeoutMs,
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
      const reply = await this.measureRequest(channelName, async () => {
        let replyParts: readonly Message[] = [];
        try {
          try {
            replyParts = await awaitWithAbort(dealer.request(parts, timeoutMs), signal);
          } catch (error) {
            if (signal?.aborted === true) throw error;
            //  Spec 32-framework-error-model:81-92 — classify the backend request
            //  terminal (deadline -> DeadlineExceeded, lost route -> Unavailable,
            //  etc.) instead of collapsing every failure to Unavailable.
            if (isZLinkBackendResultError(error) && error.operation === 'request') {
              throw new ZLinkFrameworkException(
                requestResultToPublicErrorKind(error.result),
                `Channel '${channelName}' request failed with result ${error.result}.`,
                error
              );
            }
            throw createInternalFrameworkException(
              ZLinkFrameworkInternalErrorKind.RouteNotConnected,
              `Channel '${channelName}' request failed before a reply was received.`,
              true,
              error
            );
          }
          return decodeChannelReply<TReply>(replyParts, this.codecs);
        } finally {
          closeMessages(replyParts);
        }
      });
      traceTerminal('succeeded');
      return reply;
    } catch (error) {
      traceTerminal(requestTerminalResult(error, signal));
      throw error;
    }
  }

  tryPublish(
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: unknown,
    metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
  ): ZLinkSubmitResult {
    return runWithOutboundFlow(
      this.dispatchServices.flowCreationEnabled(),
      () => this.tryPublishScoped(channelName, topic, packetName, event, metadata)
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
    return runWithOutboundFlow(
      this.dispatchServices.flowCreationEnabled(),
      () => this.publishScoped(channelName, topic, packetName, event, signal, metadata)
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
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
  ): Promise<ZLinkSubmitResult> {
    return runWithOutboundFlow(
      this.dispatchServices.flowCreationEnabled(),
      () => this.routeSubmitScoped(routerChannelId, targetNodeRid, packetName, message, signal, metadata)
    );
  }

  private async routeSubmitScoped(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
  ): Promise<ZLinkSubmitResult> {
    throwIfAborted(signal);
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
      if (isSubmitDeadline(error)) {
        this.dispatchServices.beginOutbound(ZLinkMessageFlowOutcome.Backpressured)?.trace({
          surface: ZLinkDispatchErrorSurface.RouteMeshChannel,
          messageKind: ZLinkDispatchMessageKind.Send,
          channelName: routerChannelId,
          channelRouteKind: 'route_mesh',
          packetName,
          correlationId: undefined,
          targetRid: targetNodeRid,
          result: 'backpressured'
        });
        return { status: ZLinkSubmitStatus.TimedOut };
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
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply> {
    return runWithOutboundFlow(
      this.dispatchServices.flowCreationEnabled(),
      () => this.routeRequestScoped<TReply>(
        routerChannelId,
        targetNodeRid,
        packetName,
        request,
        timeoutMs,
        signal,
        metadata
      )
    );
  }

  private async routeRequestScoped<TReply>(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply> {
    let correlationId: string | undefined;
    let router: ReturnType<ZLinkChannelSocketRegistry['routeRouter']> | undefined;
    let terminalRecorded = false;
    const traceTerminal = (result: ZLinkRuntimeMessageFlowResult): void => {
      if (terminalRecorded) return;
      terminalRecorded = true;
      this.dispatchServices.beginOutbound(
        ZLinkMessageFlowOutcome.ReplyReceived,
        result
      )?.trace({
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
      throwIfAborted(signal);
      router = this.sockets.routeRouter(routerChannelId);
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
      const reply = await this.measureRequest(routerChannelId, async () => {
        const replyParts = await awaitWithAbort(
          router!.request(targetNodeRid, parts, timeoutMs),
          signal
        );
        try {
          return decodeChannelReply<TReply>(replyParts, this.codecs);
        } finally {
          closeMessages(replyParts);
        }
      });
      traceTerminal('succeeded');
      return reply;
    } catch (error) {
      traceTerminal(requestTerminalResult(error, signal));
      if (
        this.sockets.routeMemberStatus(routerChannelId, targetNodeRid) === 'disconnected'
        && (isSubmitDeadline(error) || error instanceof ZLinkRouteDisconnectedError)
      ) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RouteNotConnected,
          `Route channel '${routerChannelId}' member '${targetNodeRid}' is not connected.`,
          true,
          error
        );
      }
      throw error;
    }
  }

  private async measureRequest<T>(channel: string, operation: () => Promise<T>): Promise<T> {
    this.pendingRequests.set(channel, this.pendingRequestCount(channel) + 1);
    try {
      return await operation();
    } finally {
      const remaining = this.pendingRequestCount(channel) - 1;
      if (remaining === 0) this.pendingRequests.delete(channel);
      else this.pendingRequests.set(channel, remaining);
    }
  }
}

/**
 * A ClientServer DEALER's request progress poller is intentionally unref'ed.
 * The public request promise is live work, however, so it needs its own
 * ref'ed handle until reply, cancellation, timeout, or failure settles it.
 */
export function keepChannelRequestAlive<T>(request: Promise<T>): Promise<T> {
  const keepalive = setTimeout(() => {}, CHANNEL_REQUEST_LOOP_KEEPALIVE_MS);
  return request.finally(() => clearTimeout(keepalive));
}

function isSubmitDeadline(error: unknown): boolean {
  return error instanceof ZLinkFrameworkException
    && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.DeadlineExceeded;
}

function publishResult(status: ZLinkSubmitStatus): ZLinkSubmitResult {
  return { status };
}

function requestTerminalResult(
  error: unknown,
  signal: AbortSignal | undefined
): ZLinkRuntimeMessageFlowResult {
  if (signal?.aborted === true) return 'cancelled';
  if (error instanceof ZLinkFrameworkException) {
    if (error.kind === ZLinkFrameworkErrorKind.ShuttingDown) return 'shutdown';
    if (error.kind === ZLinkFrameworkErrorKind.CapacityExceeded) return 'backpressured';
  }
  return 'failed';
}
