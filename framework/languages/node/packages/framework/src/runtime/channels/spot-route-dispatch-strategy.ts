import type { Message } from '../../contracts/Common/Message';
import type { RoutingId } from '../../contracts';
import type { ZLinkFrameworkRegistration } from '../configuration';
import type { ZLinkBackendSpot, ZLinkBackendSpotNode, ZLinkBackendSpotRouteBridge } from '../backend/contracts';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import {
  encodeChannelEnvelopeParts,
  type ZLinkChannelEnvelopeCodecRegistry,
  ZLinkChannelMessageKind
} from './channel-envelope';
import { codecsForFrameworkPacket } from './channel-framework-packets';
import { throwIfAborted } from '../abort';
import type { ZLinkChannelSocketRegistry } from './channel-socket-registry';
import type { ZLinkSpotRouteBridgeRawReplyRegistry } from './spot-route-bridge-raw-reply';
import { ZLinkSourceSpotRouter } from './source-spot-router';
import { ZLinkSpotRouteTargetResolver } from './spot-route-target-resolver';
import { ZLinkSpotNodeRouteTransport } from './spot-node-route-transport';
import { ZLinkRouterSocketSpotRouteTransport } from './router-socket-spot-route-transport';
import { ZLinkLocalSpotRouteTransport } from './local-spot-route-transport';
import { ZLinkSpotRouteBridgeTransport } from './spot-route-bridge-transport';

const ZLINK_SEND_DONT_WAIT = 1;

export interface ZLinkLocalSpotRouteDispatcher {
  send(
    spotId: RoutingId,
    packetName: string | undefined,
    message: unknown,
    context: {
      readonly channelName: string;
      readonly signal?: AbortSignal;
      readonly metadata?: ReadonlyMap<string, string>;
    }
  ): Promise<void>;
  request<TReply>(
    spotId: RoutingId,
    packetName: string | undefined,
    request: unknown,
    context: {
      readonly channelName: string;
      readonly signal?: AbortSignal;
      readonly metadata?: ReadonlyMap<string, string>;
    }
  ): Promise<TReply>;
}

export interface ZLinkSpotRouteDispatchStrategyOptions {
  readonly registration: ZLinkFrameworkRegistration;
  readonly sockets: ZLinkChannelSocketRegistry;
  readonly codecs: ZLinkChannelEnvelopeCodecRegistry;
  readonly spotRouteBridges: ReadonlyMap<string, ZLinkBackendSpotRouteBridge>;
  readonly rawReplies: ZLinkSpotRouteBridgeRawReplyRegistry;
  readonly localSpotRouteDispatcher?: ZLinkLocalSpotRouteDispatcher;
  readonly flowCreationEnabled?: () => boolean;
}

export class ZLinkSpotRouteDispatchStrategy {
  private readonly sourceSpotRouter: ZLinkSourceSpotRouter;
  private readonly targets: ZLinkSpotRouteTargetResolver;
  private readonly spotNodeTransport: ZLinkSpotNodeRouteTransport;
  private readonly routerSocketTransport: ZLinkRouterSocketSpotRouteTransport;
  private readonly localTransport: ZLinkLocalSpotRouteTransport;
  private readonly bridgeTransport: ZLinkSpotRouteBridgeTransport;

  constructor(private readonly options: ZLinkSpotRouteDispatchStrategyOptions) {
    this.sourceSpotRouter = new ZLinkSourceSpotRouter(options.registration.requestTimeoutMs);
    this.targets = new ZLinkSpotRouteTargetResolver(options.registration);
    this.spotNodeTransport = new ZLinkSpotNodeRouteTransport(options.registration, this.targets);
    this.routerSocketTransport = new ZLinkRouterSocketSpotRouteTransport(options.sockets);
    this.localTransport = new ZLinkLocalSpotRouteTransport(
      options.localSpotRouteDispatcher,
      options.registration.requestTimeoutMs
    );
    this.bridgeTransport = new ZLinkSpotRouteBridgeTransport(
      options.spotRouteBridges,
      options.sockets,
      options.rawReplies,
      options.registration.requestTimeoutMs
    );
  }

  setSpotNodes(spotNodes: ReadonlyMap<string, ZLinkBackendSpotNode>): void {
    this.targets.setSpotNodes(spotNodes);
  }

  spotRouteNode(routerChannelId: string): ZLinkBackendSpotNode | undefined {
    return this.targets.routeNode(routerChannelId);
  }

  canRouteChannel(routerChannelId: string): boolean {
    return this.targets.canRouteChannel(routerChannelId);
  }

  canRoutePacketChannel(routerChannelId: string): boolean {
    return this.targets.canRoutePacketChannel(routerChannelId);
  }

  async routeSendToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<void> {
    throwIfAborted(signal);
    const localSpotRouteNode = this.targets.localRouteNode(spotRouteTarget);
    if (localSpotRouteNode !== undefined) {
      await this.localTransport.send(
        spotRouteTarget.routerChannelId,
        spotRouteTarget.spotId,
        packetName,
        message,
        signal,
        metadata
      );
      return;
    }
    const parts = encodeChannelEnvelopeParts(
      ZLinkChannelMessageKind.Command,
      spotRouteTarget.routerChannelId,
      packetName,
      message,
      undefined,
      undefined,
      codecsForFrameworkPacket(packetName, this.options.codecs),
      undefined,
      this.options.flowCreationEnabled?.() ?? true,
      metadata
    ) as readonly Message[];
    if (this.targets.hasNamedSpotNode(spotRouteTarget.routerChannelId)
      && await this.spotNodeTransport.send(spotRouteTarget, parts, signal)) {
      return;
    }
    if (this.bridgeTransport.has(spotRouteTarget.routerChannelId)) {
      await this.bridgeTransport.send(spotRouteTarget, parts, signal);
      return;
    }
    if (this.targets.hasBoundRouteRouter(spotRouteTarget.routerChannelId)) {
      await this.routerSocketTransport.send(spotRouteTarget, parts, ZLINK_SEND_DONT_WAIT, signal);
      return;
    }
    if (await this.spotNodeTransport.send(spotRouteTarget, parts, signal)) {
      return;
    }
    await this.routerSocketTransport.send(spotRouteTarget, parts, ZLINK_SEND_DONT_WAIT, signal);
  }

  async routeRequestToSpot<TReply>(
    spotRouteTarget: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply> {
    throwIfAborted(signal);
    const localSpotRouteNode = this.targets.localRouteNode(spotRouteTarget);
    if (localSpotRouteNode !== undefined) {
      return this.localTransport.request<TReply>(
        spotRouteTarget.routerChannelId,
        spotRouteTarget.spotId,
        packetName,
        request,
        timeoutMs,
        signal
      );
    }
    const codecs = codecsForFrameworkPacket(packetName, this.options.codecs);
    const parts = encodeChannelEnvelopeParts(
      ZLinkChannelMessageKind.Request,
      spotRouteTarget.routerChannelId,
      packetName,
      request,
      timeoutMs,
      undefined,
      codecs,
      undefined,
      this.options.flowCreationEnabled?.() ?? true,
      metadata
    ) as readonly Message[];
    if (this.targets.hasNamedSpotNode(spotRouteTarget.routerChannelId)) {
      const namedSpotNodeRequest = this.spotNodeTransport.request<TReply>(
        spotRouteTarget,
        parts,
        codecs,
        timeoutMs,
        signal
      );
      if (namedSpotNodeRequest !== undefined) return namedSpotNodeRequest;
    }
    if (this.bridgeTransport.has(spotRouteTarget.routerChannelId)) {
      return this.bridgeTransport.request<TReply>(spotRouteTarget, parts, codecs, timeoutMs, signal);
    }
    if (this.targets.hasBoundRouteRouter(spotRouteTarget.routerChannelId)) {
      return this.routerSocketTransport.request<TReply>(
        spotRouteTarget,
        parts,
        codecs,
        ZLINK_SEND_DONT_WAIT,
        timeoutMs,
        signal
      );
    }
    const spotNodeRequest = this.spotNodeTransport.request<TReply>(
      spotRouteTarget,
      parts,
      codecs,
      timeoutMs,
      signal
    );
    if (spotNodeRequest !== undefined) {
      return spotNodeRequest;
    }
    return this.routerSocketTransport.request<TReply>(
      spotRouteTarget,
      parts,
      codecs,
      0,
      timeoutMs,
      signal
    );
  }

  async routeRequestFromSpotToSpot<TReply>(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<TReply> {
    return await this.sourceSpotRouter.request<TReply>(
      sourceSpot,
      spotRouteTarget,
      packetName,
      request,
      timeoutMs,
      signal
    );
  }

  async routeSendFromSpotToSpot(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    timeoutMs?: number
  ): Promise<void> {
    await this.sourceSpotRouter.send(
      sourceSpot,
      spotRouteTarget,
      packetName,
      message,
      signal,
      metadata,
      timeoutMs
    );
  }

  async routeRequestRawFromSpotToSpot(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<readonly Message[]> {
    return await this.sourceSpotRouter.requestRaw(sourceSpot, spotRouteTarget, request, timeoutMs, signal);
  }

  async routeRequestRawToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<readonly Message[]> {
    throwIfAborted(signal);
    if (this.targets.hasNamedSpotNode(spotRouteTarget.routerChannelId)) {
      const namedSpotNodeRequest = this.spotNodeTransport.requestRaw(
        spotRouteTarget,
        request,
        timeoutMs,
        signal
      );
      if (namedSpotNodeRequest !== undefined) return namedSpotNodeRequest;
    }
    if (this.bridgeTransport.has(spotRouteTarget.routerChannelId)) {
      return this.bridgeTransport.requestRaw(spotRouteTarget, request, timeoutMs, signal);
    }
    const spotNodeRequest = this.spotNodeTransport.requestRaw(spotRouteTarget, request, timeoutMs, signal);
    if (spotNodeRequest !== undefined) {
      return spotNodeRequest;
    }

    return this.routerSocketTransport.requestRaw(spotRouteTarget, request, timeoutMs, signal);
  }

}
