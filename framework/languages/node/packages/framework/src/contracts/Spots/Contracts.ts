import type { ZLinkActor } from '../Actors';
import type { ZLinkChannelRequestCall, ZLinkPublishCall, ZLinkSendCall } from '../Channels';
import type {
  RoutingId,
  SpotId,
  Type,
  ZLinkMessageMetadata
} from '../Common';
import type { ZLinkSpotTimerHandler } from '../Handlers';
import type { ZLinkTimer, ZLinkTimerOptions } from '../Timers';
import type {
  ZLinkEntrySpot,
  ZLinkInstanceSpot,
  ZLinkSpot,
  ZLinkSpotRelocationReadyCall
} from './ZLinkSpot';

export interface ZLinkActorHandlerRegistry {
  addHandler<THandler>(handlerType: Type<THandler>, packetName?: string): this;
}

export interface ZLinkSpotHandlerRegistry {
  addPacket<THandler>(handlerType: Type<THandler>): this;
  addSubscribe<THandler>(handlerType: Type<THandler>, channelName: string, topic: string): this;
}

export interface ZLinkInstanceSpotHandlerRegistry {
  addPacket<THandler>(handlerType: Type<THandler>): this;
}

export interface ZLinkWorkerCall<T> {
  timeoutMs(durationMs: number): ZLinkWorkerCall<T>;
  submit(signal?: AbortSignal): Promise<T>;
  yield(signal?: AbortSignal): Promise<T>;
}

export interface ZLinkSpotCommonContext<TSpot = ZLinkSpot> {
  readonly meshName: string;
  readonly spotId: SpotId;
  readonly objectGeneration: number;
  readonly nodeRid: RoutingId;
  readonly outbound: ZLinkSpotOutbound;
  addTimer<THandler extends ZLinkSpotTimerHandler<TSpot>>(
    name: string,
    periodMs: number,
    handlerType: Type<THandler>,
    options?: ZLinkTimerOptions,
    signal?: AbortSignal
  ): Promise<ZLinkTimer>;
  /**
   * Runs synchronous CPU work on the bounded worker-thread pool.
   * The function is serialized into an isolated worker, so it must be self-contained
   * and return a value supported by the structured clone algorithm.
   */
  runCpuWorker<T>(work: (signal: AbortSignal) => T): ZLinkWorkerCall<T>;
  /** Runs asynchronous I/O work without occupying a CPU worker thread. */
  runIoWorker<T>(work: (signal: AbortSignal) => Promise<T>): ZLinkWorkerCall<T>;
}

export interface ZLinkSpotContext<
  TActor extends ZLinkActor = ZLinkActor,
  TSpot extends ZLinkSpot<TActor> = ZLinkSpot<TActor>
> extends ZLinkSpotCommonContext<TSpot> {
  readonly handlers: ZLinkSpotHandlerRegistry;
  relocationReady(): ZLinkSpotRelocationReadyCall;
  leaveActor(actor: TActor, signal?: AbortSignal): Promise<void>;
  close(signal?: AbortSignal): Promise<boolean>;
}

export interface ZLinkInstanceSpotContext
  extends ZLinkSpotCommonContext<ZLinkInstanceSpot> {
  readonly handlers: ZLinkInstanceSpotHandlerRegistry;
  close(signal?: AbortSignal): Promise<boolean>;
}

export interface ZLinkEntrySpotContext<
  TActor extends ZLinkActor = ZLinkActor,
  TEntrySpot extends ZLinkEntrySpot<TActor> = ZLinkEntrySpot<TActor>
> extends ZLinkSpotCommonContext<TEntrySpot> {
  readonly handlers: ZLinkSpotHandlerRegistry;
  destroyActor(actor: TActor, signal?: AbortSignal): Promise<void>;
}

export interface ZLinkSpotOutbound {
  sendToSpot(spotId: SpotId, message: unknown): ZLinkSpotSendCall;
  requestToSpot(spotId: SpotId, request: unknown): ZLinkSpotRequestCall;
  publish(channelName: string, topic: string, event: unknown): ZLinkPublishCall;
  sendToChannel(channelName: string, message: unknown): ZLinkSendCall;
  requestToChannel(channelName: string, request: unknown): ZLinkChannelRequestCall;
}

export interface SpotRef {
  readonly spotId: SpotId;
  readonly objectGeneration: bigint;
  readonly meshName: string;
  readonly nodeRid: RoutingId;
}

export interface ZLinkSpotSendCall {
  metadata(key: string, value: string): this;
  metadata(metadata: ZLinkMessageMetadata): this;
  instanceSpot(): this;
  instanceSpot(instanceSpotType: string): this;
  inMesh(meshName: string): this;
  submit(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkSpotRequestCall {
  metadata(key: string, value: string): this;
  metadata(metadata: ZLinkMessageMetadata): this;
  instanceSpot(): this;
  instanceSpot(instanceSpotType: string): this;
  inMesh(meshName: string): this;
  timeout(timeoutMs: number): this;
  submit<TReply>(signal?: AbortSignal): Promise<TReply>;
  yield<TReply>(signal?: AbortSignal): Promise<TReply>;
}

export enum ZLinkSpotCreateState {
  Existing = 'existing',
  Created = 'created',
  Rejected = 'rejected'
}

export interface ZLinkSpotCreateResult {
  readonly spot: SpotRef;
  readonly state: ZLinkSpotCreateState;
  readonly reply?: unknown;
}

export interface ZLinkSpotInfo {
  readonly spotId: SpotId;
}

export interface ZLinkSpotManager {
  create(spotType: string): ZLinkSpotCreateCall;
  getOrCreate(spotId: SpotId, spotType: string): ZLinkSpotGetOrCreateCall;
  find(spotId: SpotId, signal?: AbortSignal): Promise<SpotRef | undefined>;
  close(spot: SpotRef, signal?: AbortSignal): Promise<boolean>;
}

export interface ZLinkSpotCreateCall {
  inMesh(meshName: string): this;
  request(request: unknown): this;
  timeout(timeoutMs: number): this;
  submit(signal?: AbortSignal): Promise<ZLinkSpotCreateResult>;
}

export interface ZLinkSpotGetOrCreateCall extends ZLinkSpotCreateCall {}
