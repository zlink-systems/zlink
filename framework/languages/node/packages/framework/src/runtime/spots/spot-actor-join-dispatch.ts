import type {
  ActorRef,
  RoutingId,
  ZLinkActor,
  ZLinkMessage,
  ZLinkMessageSerializer,
  ZLinkSpot,
  ZLinkSpotActorJoinResult
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import { ZLinkBackendSpotDispatchEvent } from '../backend/contracts';
import type {
  ZLinkBackendActorRef,
  ZLinkBackendActorRecvInfo,
  ZLinkBackendReceived,
  ZLinkBackendSpot,
  ZLinkBackendTopicMessage
} from '../backend/contracts';
import type { Message } from '../../contracts/Common/Message';
import type { RequestResult } from '../backend/runtime-values';
type RuntimeMessage = Message;
import type {
  ZLinkRemoteBoundSessionTarget
} from '../actors';
import type { ZLinkDispatchErrorReporter } from '../channels';
import { ZLinkSpotActorLifecycleDrain } from './spot-actor-lifecycle-drain';
import {
  ZLinkSpotActorPacketDrain,
  type ZLinkActorDispatchPart
} from './spot-actor-packet-drain';
import { ZLINK_RECV_DONT_WAIT } from './spot-native-flags';
import { ZLinkSpotNativeActorJoinAdmission } from './spot-native-actor-join-admission';
import { ZLinkSpotRoutedFrameDispatch } from './spot-routed-frame-dispatch';
import {
  ZLinkSpotSubscriptionDispatch
} from './spot-subscription-dispatch';
import type { ZLinkSpotHandlerRegistration } from './spot-handler-registry';
import type { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import type { ZLinkApplicationWorkClaim } from '../admission';
import type { ZLinkActorHandoffPacket, ZLinkActorHandoffResult } from '../actors/actor-handoff';
import type {
  ZLinkSpotActorTransferRuntime,
  ZLinkSpotBoundSessionRuntime
} from './spot-runtime-ports';

const ZLINK_SPOT_DISPATCH_SUBJECT_SPOT = 1;
const ZLINK_SPOT_DISPATCH_SUBJECT_CHANNEL_DEALER = 3;

/**
 * Minimal lifecycle callback surface required by the native join and actor
 * lifecycle drains. User and Entry Spots differ in ownership policy, but both
 * drains only need the callbacks declared here.
 */
interface ZLinkActorJoinAdmissionTarget {
  onActorJoin?(actorId: string, request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult>;
  onJoinedActor?(actor: ZLinkActor): Promise<void>;
  onLeaveActor?(actor: ZLinkActor): Promise<void>;
  onDisconnectActor?(actor: ZLinkActor): Promise<void>;
}

interface ZLinkSpotActorAdmissionRuntime {
  readonly resolveActor: (actorId: string) => ZLinkActor | undefined;
  readonly getTarget: () => ZLinkActorJoinAdmissionTarget;
  readonly defaultAccept: boolean;
  readonly transfer: {
    readonly kind: 'disabled';
  } | {
    readonly kind: 'enabled';
    readonly runtime: ZLinkSpotActorTransferRuntime;
  };
  readonly commitNativeActor?: (actor: ZLinkActor) => Promise<void>;
  readonly commitActorDeparture?: (actorId: string) => void;
  readonly commitTransferredActor?: (
    actor: ZLinkActor,
    backlog: readonly ZLinkActorHandoffPacket[]
  ) => Promise<readonly ZLinkActorHandoffResult[]>;
}

interface ZLinkSpotActorPacketRuntime {
  readonly handle: (
    actorId: string,
    parts: readonly Message[],
    returnResponse?: boolean,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef
  ) => Promise<unknown>;
  readonly bindRemoteSession?: (
    actor: ZLinkBackendActorRef,
    sourceNodeRid: RoutingId,
    sourceSessionRid: RoutingId,
    declaredTarget?: ZLinkRemoteBoundSessionTarget
  ) => void;
  readonly replyNoBind?: (
    info: ZLinkBackendActorRecvInfo,
    parts: readonly Message[],
    result: RequestResult
  ) => void;
}

export interface ZLinkDetachedTaskRunner {
  runDetached(taskName: string, callback: () => Promise<void>): void;
}

interface ZLinkSpotActorJoinDispatchOptions {
  readonly nativeSpot: ZLinkBackendSpot;
  readonly createReceived: () => ZLinkBackendReceived;
  readonly createTopicMessage: () => ZLinkBackendTopicMessage;
  readonly serial: ZLinkSpotSerialExecutor;
  readonly actors: ZLinkSpotActorAdmissionRuntime;
  readonly packets?: ZLinkSpotActorPacketRuntime;
  readonly boundSessionRuntime?: ZLinkSpotBoundSessionRuntime;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
  readonly claimApplicationWork?: () => ZLinkApplicationWorkClaim;
  readonly detachedTaskRunner?: ZLinkDetachedTaskRunner;
}

export class ZLinkSpotActorJoinDispatch {
  private draining = false;
  private redrainRequested = false;
  private readonly nativeSpotId: string;
  private readonly subscriptions: ZLinkSpotSubscriptionDispatch;
  private readonly actorLifecycleDrain: ZLinkSpotActorLifecycleDrain;
  private readonly actorPacketDrain: ZLinkSpotActorPacketDrain;
  private readonly nativeActorJoinAdmission: ZLinkSpotNativeActorJoinAdmission;
  private readonly routedFrames: ZLinkSpotRoutedFrameDispatch;
  private readonly nativeSpot: ZLinkBackendSpot;

  constructor(private readonly options: ZLinkSpotActorJoinDispatchOptions) {
    const actors = options.actors;
    const transfer = actors.transfer.kind === 'enabled' ? actors.transfer : undefined;
    this.nativeSpot = options.nativeSpot;
    this.nativeSpotId = String(options.nativeSpot.routingId);
    this.actorLifecycleDrain = new ZLinkSpotActorLifecycleDrain({
      nativeSpot: options.nativeSpot,
      serial: options.serial,
      resolveActor: actors.resolveActor,
      getTarget: actors.getTarget,
      commitActorDeparture: actors.commitActorDeparture,
      waitIdle: waitSpotDispatchIdle
    });
    this.actorPacketDrain = new ZLinkSpotActorPacketDrain({
      messageSerializers: options.messageSerializers,
      actorPacketHandler: options.packets?.handle,
      bindRemoteActorSession: options.packets?.bindRemoteSession,
      replyActorNoBind: options.packets?.replyNoBind,
      waitIdle: waitSpotDispatchIdle
    });
    this.nativeActorJoinAdmission = new ZLinkSpotNativeActorJoinAdmission({
      nativeSpot: options.nativeSpot,
      serial: options.serial,
      resolveActor: actors.resolveActor,
      getTarget: actors.getTarget,
      defaultAccept: actors.defaultAccept,
      commitAcceptedActor: actors.commitNativeActor,
      messageSerializers: options.messageSerializers,
      dispatchErrors: options.dispatchErrors
    });
    this.routedFrames = new ZLinkSpotRoutedFrameDispatch({
      nativeSpot: options.nativeSpot,
      createReceived: options.createReceived,
      nativeSpotId: this.nativeSpotId,
      serial: options.serial,
      resolveActor: actors.resolveActor,
      getTarget: () => actors.getTarget() as ZLinkActorJoinAdmissionTarget & ZLinkSpot,
      defaultAccept: actors.defaultAccept,
      routedActorTransferProvider: transfer?.runtime.materializeRoutedActor.bind(transfer.runtime),
      commitTransferredActor: actors.commitTransferredActor,
      actorPacketHandler: options.packets?.handle,
      bindRemoteSession: options.packets?.bindRemoteSession === undefined
        ? undefined
        : (actor, sourceNodeRid, sourceSessionRid, declaredTarget) =>
            options.packets!.bindRemoteSession!(
              {
                actorId: actor.actorId,
                generation: actor.objectGeneration,
                nodeRid: actor.nodeRid
              },
              sourceNodeRid,
              sourceSessionRid,
              declaredTarget
            ),
      routedBoundSessionReceiver: options.boundSessionRuntime?.receiveRoutedBoundSession.bind(options.boundSessionRuntime),
      routedBoundSessionResponseReceiver: options.boundSessionRuntime?.receiveRoutedBoundSessionResponse.bind(options.boundSessionRuntime),
      routedBoundSessionErrorReceiver: options.boundSessionRuntime?.receiveRoutedBoundSessionError.bind(options.boundSessionRuntime),
      routedBoundSessionOwnershipReceiver: options.boundSessionRuntime?.receiveRemoteBoundSessionOwnership.bind(options.boundSessionRuntime),
      routedBoundSessionSealReceiver: options.boundSessionRuntime?.receiveRemoteBoundSessionSeal.bind(options.boundSessionRuntime),
      actorPacketTargetProvider: options.boundSessionRuntime?.actorPacketTargetForState.bind(options.boundSessionRuntime),
      messageSerializers: options.messageSerializers,
      providerResolver: options.providerResolver,
      dispatchErrors: options.dispatchErrors,
      claimApplicationWork: options.claimApplicationWork,
      waitIdle: waitSpotDispatchIdle
    });
    this.subscriptions = new ZLinkSpotSubscriptionDispatch({
      nativeSpot: options.nativeSpot,
      createTopicMessage: options.createTopicMessage,
      serial: options.serial,
      getTarget: () => actors.getTarget() as ZLinkSpot,
      messageSerializers: options.messageSerializers,
      providerResolver: options.providerResolver,
      dispatchErrors: options.dispatchErrors,
      waitIdle: waitSpotDispatchIdle
    });
  }

  configureSubscriptions(registrations: readonly ZLinkSpotHandlerRegistration[]): void {
    this.subscriptions.configure(registrations);
    this.routedFrames.configurePacketHandlers(registrations);
  }

  async dispatchSubscriptionRecord(
    topic: string,
    parts: readonly RuntimeMessage[],
    sourceRid: RoutingId | null
  ): Promise<void> {
    await this.subscriptions.dispatchRecord(topic, parts, sourceRid);
  }

  dispose(): void {
    this.actorPacketDrain.dispose();
  }

  attach(): void {
    if (typeof this.nativeSpot.setDispatchHandler !== 'function') {
      return;
    }
    this.nativeSpot.setDispatchHandler((info) => {
      if (info.event === ZLinkBackendSpotDispatchEvent.ActorJoinReadable) {
        this.runDetached('spot actor join drain', () => this.drain());
        return;
      }
      if (info.event === ZLinkBackendSpotDispatchEvent.SubscribeReadable) {
        this.runDetached('spot subscription drain', () => this.subscriptions.drain());
        return;
      }
      if (info.event === ZLinkBackendSpotDispatchEvent.ChannelReplyReadable) {
        if (info.subjectKind === ZLINK_SPOT_DISPATCH_SUBJECT_CHANNEL_DEALER &&
            info.subjectHandle !== undefined) {
          this.nativeSpot.drainChannelReply(info.subjectHandle);
          return;
        }
        if (info.subjectKind === ZLINK_SPOT_DISPATCH_SUBJECT_SPOT ||
            info.subjectKind === undefined) {
          this.nativeSpot.drainReply();
        }
        return;
      }
      if (info.event === ZLinkBackendSpotDispatchEvent.RoutedReadable) {
        if (info.routed !== undefined && info.routed !== null) {
          this.runDetached('spot routed frame dispatch', () => this.routedFrames.dispatchFromEvent(info.routed!));
        }
        return;
      }
      if (info.event === ZLinkBackendSpotDispatchEvent.ActorReadable) {
        this.runDetached('spot actor packet drain', () => this.actorPacketDrain.drain(info as unknown as {
          recvActorPart(flags?: number): ZLinkActorDispatchPart | null;
        }));
        return;
      }
      if (info.event === ZLinkBackendSpotDispatchEvent.ActorLifecycleReadable) {
        this.runDetached('spot actor lifecycle drain', () => this.actorLifecycleDrain.drain());
      }
    });
  }

  private runDetached(taskName: string, callback: () => Promise<void>): void {
    if (this.options.detachedTaskRunner !== undefined) {
      this.options.detachedTaskRunner.runDetached(taskName, callback);
      return;
    }
    void callback().catch(() => undefined);
  }

  private async drain(): Promise<void> {
    if (this.draining) {
      this.redrainRequested = true;
      return;
    }
    this.draining = true;
    try {
      do {
        this.redrainRequested = false;
        await this.drainAvailableActorJoins();
        // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
      } while (this.redrainRequested);
    } finally {
      this.draining = false;
    }
  }

  private async drainAvailableActorJoins(): Promise<void> {
    for (;;) {
      const request = this.nativeSpot.recvActorJoin(ZLINK_RECV_DONT_WAIT);
      if (request === null) {
        await waitSpotDispatchIdle();
        return;
      }
      await this.nativeActorJoinAdmission.admit(request);
    }
  }
}

function waitSpotDispatchIdle(): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, 5));
}
