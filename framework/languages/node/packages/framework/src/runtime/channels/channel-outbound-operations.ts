import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  internalFrameworkErrorKind
} from '../framework-errors-internal';
import type { Message } from '../../contracts/Common/Message';
import {
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import {
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { ZLinkBackendSendFlags } from '../backend/contracts';
import { throwIfAborted } from '../abort';
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
import { ZLinkFrameworkException } from '../../contracts';
import { ZLinkRouteDisconnectedError } from './route-disconnected-error';

const ZLINK_SEND_DONT_WAIT = 1 as ZLinkBackendSendFlags;

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

  /**
   * Try-style fast-fail counterpart of {@link send}. It reports the current selection instead of
   * waiting, so the bounded readiness wait that
   * `framework/doc/framework/common/spec/08-channel-messaging.ko.md` §3.2 puts on a call belongs to
   * the asynchronous {@link send} the public `sendToChannel` path uses, not here.
   */
  trySend(
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    metadata: ReadonlyMap<string, string> = new Map()
  ): ZLinkSubmitResult {
    const dealer = this.sockets.clientDealerForOutbound(channelName);
    if (dealer === undefined) {
      return { status: ZLinkSubmitStatus.TargetNotFound };
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
    const accepted = this.sockets.requireSubmitter(dealer).trySubmitCommand(
      () => dealer.send(parts, ZLINK_SEND_DONT_WAIT),
      () => closeMessages(parts)
    );
    if (!accepted) {
      return { status: ZLinkSubmitStatus.Backpressured };
    }
    this.traceSend(channelName, packetName);
    return { status: ZLinkSubmitStatus.Submitted };
  }

  async send(
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = new Map()
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
      await this.sockets.requireSubmitter(dealer).submitCommand(
        () => dealer.send(parts, ZLINK_SEND_DONT_WAIT),
        signal,
        () => closeMessages(parts)
      );
    } catch (error) {
      if (isSubmitDeadline(error)) {
        return { status: ZLinkSubmitStatus.TimedOut };
      }
      throw error;
    }
    this.traceSend(channelName, packetName);
    return { status: ZLinkSubmitStatus.Submitted };
  }

  private traceSend(
    channelName: string,
    packetName: string | undefined
  ): void {
    this.dispatchServices.traceOutbound(ZLinkMessageFlowOutcome.Sent, () => ({
      surface: ZLinkDispatchErrorSurface.Channel,
      messageKind: ZLinkDispatchMessageKind.Send,
      channelName,
      packetName,
      correlationId: undefined,
    }));
  }

  async request<TReply>(
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply> {
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
    const correlationId = newChannelCorrelationId();
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
    this.dispatchServices.traceOutbound(ZLinkMessageFlowOutcome.Sent, () => ({
      surface: ZLinkDispatchErrorSurface.Channel,
      messageKind: ZLinkDispatchMessageKind.Request,
      channelName,
      packetName,
      correlationId
    }));
    return this.measureRequest(channelName, () => this.sockets.requireSubmitter(dealer).submitRequest(
      (resolve, reject) => {
        try {
          const submitted = dealer.request(
            parts,
            (result, replyParts) => {
              try {
                if (result !== 0) {
                  reject(createInternalFrameworkException(
                    ZLinkFrameworkInternalErrorKind.RouteNotConnected,
                    `Channel '${channelName}' request failed with result ${result}.`,
                    true
                  ));
                  return;
                }
                const reply = decodeChannelReply<TReply>(replyParts as readonly Message[], this.codecs);
                this.dispatchServices.traceOutbound(ZLinkMessageFlowOutcome.ReplyReceived, () => ({
                  surface: ZLinkDispatchErrorSurface.Channel,
                  messageKind: ZLinkDispatchMessageKind.Request,
                  channelName,
                  packetName,
                  correlationId
                }));
                resolve(reply);
              } catch (error) {
                reject(error);
              } finally {
                closeMessages(replyParts as readonly Message[]);
              }
            },
            ZLINK_SEND_DONT_WAIT,
            timeoutMs
          );
          return submitted;
        } catch (error) {
          reject(createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RouteNotConnected,
            `Channel '${channelName}' request failed before a reply was received.`,
            true,
            error
          ));
          return true;
        }
      },
      signal,
      timeoutMs,
      () => closeMessages(parts)
    ));
  }

  tryPublish(
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: unknown,
    metadata: ReadonlyMap<string, string> = new Map()
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
      this.dispatchServices.flowCreationEnabled(),
      metadata
    ) as readonly Message[];
    const accepted = this.sockets.requireSubmitter(publisher).trySubmitCommand(
      () => publisher.publish(topic, parts, ZLINK_SEND_DONT_WAIT),
      () => closeMessages(parts)
    );
    if (!accepted) {
      return publishResult(ZLinkSubmitStatus.Backpressured);
    }
    return publishResult(ZLinkSubmitStatus.Submitted);
  }

  async publish(
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: unknown,
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = new Map()
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
      this.dispatchServices.flowCreationEnabled(),
      metadata
    ) as readonly Message[];
    try {
      await this.sockets.requireSubmitter(publisher).submitCommand(
        () => publisher.publish(topic, parts, ZLINK_SEND_DONT_WAIT),
        signal,
        () => closeMessages(parts)
      );
    } catch (error) {
      if (isSubmitDeadline(error)) {
        return publishResult(ZLinkSubmitStatus.TimedOut);
      }
      throw error;
    }
    return publishResult(ZLinkSubmitStatus.Submitted);
  }

  tryRouteSubmit(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    metadata: ReadonlyMap<string, string> = new Map()
  ): ZLinkSubmitResult {
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
    const accepted = this.sockets.requireSubmitter(router).trySubmitCommand(
      () => router.send(targetNodeRid, parts, ZLINK_SEND_DONT_WAIT),
      () => closeMessages(parts)
    );
    if (!accepted) {
      return { status: ZLinkSubmitStatus.Backpressured };
    }
    this.traceRouteSend(routerChannelId, targetNodeRid, packetName);
    return { status: ZLinkSubmitStatus.Submitted };
  }

  async routeSubmit(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = new Map()
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
      await this.sockets.requireSubmitter(router).submitCommand(
        () => router.send(targetNodeRid, parts, ZLINK_SEND_DONT_WAIT),
        signal,
        () => closeMessages(parts)
      );
    } catch (error) {
      if (isSubmitDeadline(error)) {
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
    this.dispatchServices.traceOutbound(ZLinkMessageFlowOutcome.Sent, () => ({
      surface: ZLinkDispatchErrorSurface.RouteMeshChannel,
      messageKind: ZLinkDispatchMessageKind.Send,
      channelName: routerChannelId,
      packetName,
      correlationId: undefined,
      sourceRid: targetNodeRid
    }));
  }

  async routeRequest<TReply>(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply> {
    throwIfAborted(signal);
    const router = this.sockets.routeRouter(routerChannelId);
    if (this.sockets.routeMemberStatus(routerChannelId, targetNodeRid) === 'missing') {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
        `Route channel '${routerChannelId}' has no member '${targetNodeRid}'.`
      );
    }
    const correlationId = newChannelCorrelationId();
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
    this.dispatchServices.traceOutbound(ZLinkMessageFlowOutcome.Sent, () => ({
      surface: ZLinkDispatchErrorSurface.RouteMeshChannel,
      messageKind: ZLinkDispatchMessageKind.Request,
      channelName: routerChannelId,
      packetName,
      correlationId,
      sourceRid: targetNodeRid
    }));
    try {
      return await this.measureRequest(routerChannelId, () => this.sockets.requireSubmitter(router).submitRequest(
      (resolve, reject) => {
        let submitted: boolean;
        try {
          submitted = router.request(
            targetNodeRid,
            parts,
            (result, replyParts) => {
              try {
                if (result !== 0) {
                  reject(createInternalFrameworkException(
                    ZLinkFrameworkInternalErrorKind.RouteNotConnected,
                    `Route channel '${routerChannelId}' request failed with result ${result}.`,
                    true
                  ));
                  return;
                }
                const reply = decodeChannelReply<TReply>(replyParts as readonly Message[], this.codecs);
                this.dispatchServices.traceOutbound(ZLinkMessageFlowOutcome.ReplyReceived, () => ({
                  surface: ZLinkDispatchErrorSurface.RouteMeshChannel,
                  messageKind: ZLinkDispatchMessageKind.Request,
                  channelName: routerChannelId,
                  packetName,
                  correlationId,
                  sourceRid: targetNodeRid
                }));
                resolve(reply);
              } catch (error) {
                reject(error);
              } finally {
                closeMessages(replyParts as readonly Message[]);
              }
            },
            ZLINK_SEND_DONT_WAIT,
            timeoutMs
          );
        } catch (error) {
          reject(createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RouteNotConnected,
            `Route channel '${routerChannelId}' request failed before a reply was received.`,
            true,
            error
          ));
          return true;
        }
        if (!submitted) {
          reject(createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RouteNotConnected,
            `Route channel '${routerChannelId}' is not ready for request.`,
            true
          ));
        }
        return submitted;
      },
      signal,
      timeoutMs,
      () => closeMessages(parts)
      ));
    } catch (error) {
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

function isSubmitDeadline(error: unknown): boolean {
  return error instanceof ZLinkFrameworkException
    && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.DeadlineExceeded;
}

function publishResult(status: ZLinkSubmitStatus): ZLinkSubmitResult {
  return { status };
}
