import type {
  ZLinkActor,
  ZLinkMessage,
  ZLinkMessageSerializer,
  ZLinkSpot,
  ZLinkSpotActorJoinResult
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { RoutingId } from '../../contracts';
import type { ActorRef } from '../../contracts/Common/ActorRef';
import type { Message } from '../../contracts/Common/Message';
import type {
  ZLinkRemoteActorPacketTarget,
  ZLinkRemoteBoundSessionTarget
} from '../actors';
import type {
  ZLinkBackendReceived,
  ZLinkBackendSpot
} from '../backend/contracts';
import type { ZLinkDispatchErrorReporter } from '../channels';
import type { ZLinkChannelEnvelopeCodecRegistry } from '../channels/channel-envelope';
import { decodeRemoteActorPacketRelay } from './spot-remote-route-codec';
import { ZLINK_RECV_DONT_WAIT } from './spot-native-flags';
import { ZLinkSpotActorPacketRelayDispatch } from './spot-actor-packet-relay-dispatch';
import type { ZLinkActorResponseOptions } from './spot-actor-packet-dispatch';
import { ZLinkSpotRoutePacketDispatch } from './spot-route-packet-dispatch';
import { ZLinkSpotRoutedActorAdmission } from './spot-routed-actor-admission';
import { ZLinkSpotRoutedBoundSessionDispatch } from './spot-routed-bound-session-dispatch';
import type { ZLinkSpotHandlerRegistration } from './spot-handler-registry';
import type { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import type { ZLinkApplicationWorkClaim } from '../admission';
import type { ZLinkRoutedActorTransferProvider } from './spot-remote-codec';
import type { ZLinkActorHandoffPacket, ZLinkActorHandoffResult } from '../actors/actor-handoff';

interface ZLinkRoutedFrameAdmissionTarget {
  onActorJoin?(actorId: string, request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult>;
}

interface ZLinkSpotRoutedFrameDispatchOptions {
  readonly nativeSpot: ZLinkBackendSpot;
  readonly createReceived: () => ZLinkBackendReceived;
  readonly nativeSpotId: string;
  readonly serial: ZLinkSpotSerialExecutor;
  readonly resolveActor: (actorId: string) => ZLinkActor | undefined;
  readonly getTarget: () => ZLinkRoutedFrameAdmissionTarget & ZLinkSpot;
  readonly defaultAccept: boolean;
  readonly routedActorTransferProvider?: ZLinkRoutedActorTransferProvider;
  readonly commitTransferredActor?: (
    actor: ZLinkActor,
    backlog: readonly ZLinkActorHandoffPacket[]
  ) => Promise<readonly ZLinkActorHandoffResult[]>;
  readonly actorPacketHandler?: (
    actorId: string,
    parts: readonly Message[],
    returnResponse?: boolean,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef
  ) => Promise<unknown>;
  readonly routedBoundSessionReceiver?: (
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    actorRef?: ActorRef,
    actorPacketTarget?: unknown,
    signal?: AbortSignal
  ) => Promise<void>;
  readonly routedBoundSessionResponseReceiver?: (
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    replyOptions: ZLinkActorResponseOptions,
    actorPacketTarget?: unknown,
    signal?: AbortSignal
  ) => Promise<void>;
  readonly routedBoundSessionErrorReceiver?: (
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>,
    actorPacketTarget?: unknown,
    signal?: AbortSignal
  ) => Promise<void>;
  readonly routedBoundSessionOwnershipReceiver?: (payload: unknown) => Promise<{
    readonly actorId: string;
    readonly actorGeneration: string;
    readonly actorOwnershipGeneration: string;
    readonly bindingGeneration: string;
    readonly targetOwnerLeaseGeneration: string;
    readonly acceptedHighWater: string;
    readonly sealId: string;
  }>;
  readonly routedBoundSessionSealReceiver?: (payload: unknown) => Promise<{
    readonly actorId: string;
    readonly sealId: string;
    readonly acceptedHighWater: string;
  }>;
  readonly actorPacketTargetProvider?: (actorId: string) => ZLinkRemoteActorPacketTarget | undefined;
  readonly bindRemoteSession?: (
    actor: ActorRef,
    sourceNodeRid: RoutingId,
    sourceSessionRid: RoutingId,
    declaredTarget?: ZLinkRemoteBoundSessionTarget
  ) => void;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
  readonly claimApplicationWork?: () => ZLinkApplicationWorkClaim;
  readonly waitIdle: () => Promise<void>;
}

export class ZLinkSpotRoutedFrameDispatch {
  private routeDraining = false;
  private readonly packetHandlers = new Map<string, ZLinkSpotHandlerRegistration[]>();
  private readonly routedBoundSessionDispatch: ZLinkSpotRoutedBoundSessionDispatch;
  private readonly actorPacketRelayDispatch: ZLinkSpotActorPacketRelayDispatch;
  private readonly routePacketDispatch: ZLinkSpotRoutePacketDispatch;
  private readonly routedActorAdmission: ZLinkSpotRoutedActorAdmission;

  constructor(private readonly options: ZLinkSpotRoutedFrameDispatchOptions) {
    this.routedBoundSessionDispatch = new ZLinkSpotRoutedBoundSessionDispatch({
      channelCodecs: () => this.channelCodecs(),
      routedBoundSessionReceiver: options.routedBoundSessionReceiver,
      routedBoundSessionResponseReceiver: options.routedBoundSessionResponseReceiver,
      routedBoundSessionErrorReceiver: options.routedBoundSessionErrorReceiver,
      routedBoundSessionOwnershipReceiver: options.routedBoundSessionOwnershipReceiver,
      routedBoundSessionSealReceiver: options.routedBoundSessionSealReceiver,
      dispatchErrors: options.dispatchErrors
    });
    this.actorPacketRelayDispatch = new ZLinkSpotActorPacketRelayDispatch({
      actorPacketHandler: options.actorPacketHandler,
      actorPacketTargetProvider: options.actorPacketTargetProvider,
      bindRemoteSession: options.bindRemoteSession
    });
    this.routePacketDispatch = new ZLinkSpotRoutePacketDispatch({
      packetHandlers: this.packetHandlers,
      nativeSpotId: options.nativeSpotId,
      serial: options.serial,
      getTarget: options.getTarget,
      providerResolver: options.providerResolver,
      messageSerializers: options.messageSerializers,
      dispatchErrors: options.dispatchErrors,
      claimApplicationWork: options.claimApplicationWork
    });
    this.routedActorAdmission = new ZLinkSpotRoutedActorAdmission({
      serial: options.serial,
      getTarget: options.getTarget,
      defaultAccept: options.defaultAccept,
      routedActorTransferProvider: options.routedActorTransferProvider,
      commitTransferredActor: options.commitTransferredActor,
      messageSerializers: options.messageSerializers
    });
  }

  configurePacketHandlers(registrations: readonly ZLinkSpotHandlerRegistration[]): void {
    for (const registration of registrations) {
      if (registration.kind !== 'packet') {
        continue;
      }
      const packetName = registration.packetName ?? registration.handlerType.name;
      const existing = this.packetHandlers.get(packetName) ?? [];
      existing.push(registration);
      this.packetHandlers.set(packetName, existing);
    }
  }

  async drain(received: ZLinkBackendReceived | undefined = undefined, retryDeadlineMs = Date.now()): Promise<void> {
    if (this.routeDraining) {
      if (received !== undefined) {
        try {
          await this.dispatch(received);
        } finally {
          received.close();
        }
      }
      return;
    }
    this.routeDraining = true;
    try {
      if (received !== undefined) {
        try {
          await this.dispatch(received);
        } finally {
          received.close();
        }
        received = undefined;
      }
      for (;;) {
        received ??= this.options.createReceived();
        try {
          if (!this.options.nativeSpot.recvRoute(received, ZLINK_RECV_DONT_WAIT)) {
            received.close();
            await this.options.waitIdle();
            return;
          }
        } catch (error) {
          if (isRouteRecvRetryable(error)) {
            closeReceivedQuietly(received);
            if (Date.now() < retryDeadlineMs) {
              setTimeout(() => void this.drain(undefined, retryDeadlineMs), 10);
            }
            return;
          }
          received.close();
          throw error;
        }
        try {
          await this.dispatch(received);
        } finally {
          received.close();
        }
        received = this.options.createReceived();
      }
    } finally {
      this.routeDraining = false;
    }
  }

  async dispatchFromEvent(received: ZLinkBackendReceived): Promise<void> {
    try {
      await this.dispatch(received);
    } finally {
      received.close();
    }
  }

  private channelCodecs(): ZLinkChannelEnvelopeCodecRegistry | undefined {
    return this.options.messageSerializers === undefined
      ? undefined
      : { serializers: this.options.messageSerializers };
  }

  private async dispatch(received: ZLinkBackendReceived): Promise<void> {
    if (received.parts.length < 1) {
      return;
    }
    if (await this.routedBoundSessionDispatch.dispatch(received)) {
      return;
    }
    const actorPacketRelay = decodeRemoteActorPacketRelay(received.parts, this.channelCodecs());
    if (actorPacketRelay !== undefined && await this.actorPacketRelayDispatch.dispatch(received, actorPacketRelay)) {
      return;
    }
    if (await this.routePacketDispatch.dispatch(received)) {
      return;
    }
    await this.routedActorAdmission.admit(received);
  }
}

function isRouteRecvRetryable(error: unknown): boolean {
  return typeof error === 'object' &&
    error !== null &&
    [201, 202, 204].includes(Number((error as { result?: unknown }).result));
}

function closeReceivedQuietly(received: ZLinkBackendReceived): void {
  try {
    received.close();
  } catch {
  }
}
