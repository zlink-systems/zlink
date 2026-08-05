import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendSpot } from '../backend/contracts';
import type { ZLinkSubmitResult } from '../messaging/submission-result';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';

export interface ZLinkActorRoutedJoinTransport {
  canRouteChannel?(routerChannelId: string): boolean;
  canRoutePacketChannel?(routerChannelId: string): boolean;
  request<TReply>(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<TReply>;
  submit?(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult>;
  submitInfrastructure?(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult>;
  sendToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    message: unknown,
    options: { readonly packetName?: string; readonly signal?: AbortSignal }
  ): Promise<ZLinkSubmitResult>;
  requestRawToSpot?(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    options: { readonly timeoutMs?: number; readonly signal?: AbortSignal }
  ): Promise<readonly Message[]>;
  requestToSpot<TReply = unknown>(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: unknown,
    options: { readonly packetName?: string; readonly timeoutMs?: number; readonly signal?: AbortSignal }
  ): Promise<TReply>;
  requestFromSpotToSpot?<TReply = unknown>(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: unknown,
    options: { readonly packetName?: string; readonly timeoutMs?: number; readonly signal?: AbortSignal }
  ): Promise<TReply>;
  requestRawFromSpotToSpot?(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    options: { readonly timeoutMs?: number; readonly signal?: AbortSignal }
  ): Promise<readonly Message[]>;
}
