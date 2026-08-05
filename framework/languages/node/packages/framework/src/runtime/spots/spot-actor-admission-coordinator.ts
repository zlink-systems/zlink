import type { ActorRef, ZLinkActor } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkMessageFollowOrigin } from '../foundation/service-runtime-contracts';
import type { ZLinkBackendSpot } from '../backend/contracts';
import {
  type ZLinkActorHandoffPacket,
  type ZLinkActorHandoffResult,
  type ZLinkRemoteBoundSessionTarget
} from '../actors';
import { replayActorHandoffBacklog } from '../actors/actor-handoff';
import { routingIdsEqual } from '../routing-id';
import type { ZLinkSpotActivationLifecycleOptions } from './spot-activation';
import { ZLinkSpotActivation } from './spot-activation-state';
import { ZLinkSpotActorJoinDispatch } from './spot-actor-join-dispatch';
import { ZLinkSpotActorPacketDispatch } from './spot-actor-packet-dispatch';
import type { ZLinkNativeActorJoinSnapshot } from './spot-runtime-ports';

type AdmissionOptions = Pick<ZLinkSpotActivationLifecycleOptions,
  | 'actorHandoffRuntime'
  | 'admission'
  | 'actorResolver'
  | 'actorTransferRuntime'
  | 'boundSessionRuntime'
  | 'detachedTaskRunner'
  | 'dispatchErrors'
  | 'locationClaim'
  | 'messageSerializers'
  | 'nativeSpotNodeProvider'
  | 'createReceived'
  | 'createTopicMessage'
  | 'providerResolver'
  | 'runtimeEventPublisher'
>;

export class ZLinkSpotActorAdmissionCoordinator {
  constructor(private readonly options: AdmissionOptions) {}

  async dispatchActorPacket(
    activation: ZLinkSpotActivation,
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: (response: unknown) => Promise<void> | void,
    messageFollowOrigin?: ZLinkMessageFollowOrigin
  ): Promise<unknown> {
    const handoff = this.options.actorHandoffRuntime?.capture(
      actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef,
      undefined,
      messageFollowOrigin,
      (replayedParts, replayReturnResponse, replayRemoteBoundSessionTarget, replayFallbackActorRef) =>
        this.dispatchActorPacketDirect(
          activation,
          actorId,
          replayedParts,
          replayReturnResponse,
          replayRemoteBoundSessionTarget,
          replayFallbackActorRef
        )
    );
    if (handoff !== undefined) return await handoff;
    return await this.dispatchActorPacketDirect(
      activation,
      actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef,
      requestTerminal
    );
  }

  attachNativeActorJoinDispatch(
    activation: ZLinkSpotActivation,
    nativeSpot: ZLinkBackendSpot | undefined
  ): ZLinkSpotActorJoinDispatch | undefined {
    if (nativeSpot === undefined) return undefined;
    const nativeDispatch = new ZLinkSpotActorJoinDispatch({
      nativeSpot,
      createReceived: requireBackendValueFactory(this.options.createReceived, 'Received'),
      createTopicMessage: requireBackendValueFactory(this.options.createTopicMessage, 'TopicMessage'),
      serial: activation.serial,
      actors: {
        resolveActor: (actorId) => activation.hasDepartedActor(actorId)
          ? undefined
          : activation.resolveJoinedActor(actorId) ?? this.options.actorResolver?.(actorId),
        getTarget: () => activation.spot,
        defaultAccept: false,
        transfer: this.options.actorTransferRuntime === undefined ? { kind: 'disabled' } : {
          kind: 'enabled',
          runtime: this.options.actorTransferRuntime
        },
        commitNativeActor: (actor) => this.commitNativeActorTransaction(activation, actor),
        commitActorDeparture: (actorId) => activation.commitActorDeparture(actorId),
        commitTransferredActor: (actor, backlog) => this.commitTransferredActorTransaction(activation, actor, backlog)
      },
      packets: {
        handle: (actorId, parts, returnResponse, remoteBoundSessionTarget, fallbackActorRef) =>
          this.dispatchActorPacket(
            activation,
            actorId,
            parts,
            returnResponse,
            remoteBoundSessionTarget,
            fallbackActorRef
          ),
        bindRemoteSession: (actor, sourceNodeRid, sourceSessionRid) => {
          const node = this.options.nativeSpotNodeProvider?.(activation.meshName);
          if (node === undefined || routingIdsEqual(sourceNodeRid, node.routingId)) return;
          const target = this.options.boundSessionRuntime?.resolveRemoteBoundSessionTarget(sourceNodeRid, sourceSessionRid);
          if (target !== undefined) {
            this.options.boundSessionRuntime?.rememberRemoteBoundSessionTarget(actor.actorId, target);
          }
          node.bindRemoteActorSession(actor, sourceNodeRid, sourceSessionRid);
        },
        replyNoBind: (info, parts, result) =>
          this.options.nativeSpotNodeProvider?.(activation.meshName)?.replyActorNoBind(info, parts, result)
      },
      boundSessionRuntime: this.options.boundSessionRuntime,
      messageSerializers: this.options.messageSerializers,
      providerResolver: this.options.providerResolver,
      dispatchErrors: this.options.dispatchErrors,
      claimApplicationWork: this.options.admission === undefined
        ? undefined
        : () => this.options.admission!.claim(activation.meshName, 'Spot route dispatch'),
      detachedTaskRunner: this.options.detachedTaskRunner
    });
    nativeDispatch.attach();
    return nativeDispatch;
  }

  private async dispatchActorPacketDirect(
    activation: ZLinkSpotActivation,
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: (response: unknown) => Promise<void> | void
  ): Promise<unknown> {
    return await activation.executeActor(actorId, async (actorSerial) =>
      new ZLinkSpotActorPacketDispatch({
        spot: activation.spot,
        spotId: () => String(activation.spotId),
        registry: activation.actorHandlers,
        serial: actorSerial,
        resolveActor: (targetActorId) => activation.hasDepartedActor(targetActorId)
          ? undefined
          : activation.resolveJoinedActor(targetActorId) ?? this.options.actorResolver?.(targetActorId),
        actorLeft: (targetActorId) => activation.hasDepartedActor(targetActorId),
        onRemoteBoundSessionTarget: (targetActorId, target) =>
          this.options.boundSessionRuntime?.rememberRemoteBoundSessionTarget(targetActorId, target),
        onDisconnectActor: (actor) =>
          activation.serial.execute(() => activation.spot.onDisconnectActor?.(actor)),
        actorResponseSender: this.options.boundSessionRuntime?.sendActorResponse.bind(this.options.boundSessionRuntime),
        actorErrorSender: this.options.boundSessionRuntime?.sendActorError.bind(this.options.boundSessionRuntime),
        providerResolver: this.options.providerResolver,
        messageSerializers: this.options.messageSerializers,
        dispatchErrors: this.options.dispatchErrors
      }).dispatch(
        actorId,
        parts,
        returnResponse,
        remoteBoundSessionTarget,
        fallbackActorRef,
        requestTerminal
      )
    );
  }

  private async commitNativeActorTransaction(
    activation: ZLinkSpotActivation,
    actor: ZLinkActor
  ): Promise<void> {
    const transfer = this.options.actorTransferRuntime;
    let snapshot: ZLinkNativeActorJoinSnapshot | undefined;
    let rollbackMembership: (() => void) | undefined;
    let routeSwitchStarted = false;
    try {
      snapshot = await transfer?.claimNativeActorLocation(
        actor,
        activation.spotId,
        activation.meshName
      );
      transfer?.commitRoutedActor(actor, activation.spotId, activation.spot);
      rollbackMembership = activation.commitActorJoin(actor);
      activation.beginActorTransfer(actor.context.actorId);
      await activation.serial.execute(() => activation.spot.onJoinedActor(actor));
      routeSwitchStarted = true;
      await transfer?.publishRoutedActorOwnership(actor);
      await transfer?.openRoutedActorSession(actor);
      activation.cancelActorTransfer(actor.context.actorId);
    } catch (error) {
      if (routeSwitchStarted) throw error;
      rollbackMembership?.();
      try {
        if (snapshot !== undefined) await transfer?.rollbackNativeActorJoin(actor, snapshot);
      } catch (rollbackError) {
        throw new AggregateError([error, rollbackError], 'Native actor admission and rollback both failed.');
      }
      throw error;
    }
  }

  private async commitTransferredActorTransaction(
    activation: ZLinkSpotActivation,
    actor: ZLinkActor,
    backlog: readonly ZLinkActorHandoffPacket[]
  ): Promise<readonly ZLinkActorHandoffResult[]> {
    const transfer = this.options.actorTransferRuntime;
    let routeSwitchStarted = false;
    try {
      transfer?.commitRoutedActor(actor, activation.spotId, activation.spot);
      activation.commitActorJoin(actor);
      activation.beginActorTransfer(actor.context.actorId);
      await activation.serial.execute(() => activation.spot.onJoinedActor(actor));
      const results = backlog.length === 0
        ? []
        : await this.replayActorBacklog(activation, actor, backlog);
      await transfer?.claimRoutedActorLocation(
        actor,
        activation.spotId,
        activation.meshName
      );
      routeSwitchStarted = true;
      await transfer?.publishRoutedActorOwnership(actor);
      await transfer?.openRoutedActorSession(actor);
      activation.cancelActorTransfer(actor.context.actorId);
      return results;
    } catch (error) {
      if (routeSwitchStarted) throw error;
      activation.commitActorDeparture(actor.context.actorId);
      transfer?.clearRoutedActor(actor);
      try {
        await transfer?.rollbackRoutedActor(actor);
      } catch (rollbackError) {
        throw new AggregateError([error, rollbackError], 'Actor admission and rollback both failed.');
      }
      throw error;
    }
  }

  private async replayActorBacklog(
    activation: ZLinkSpotActivation,
    actor: ZLinkActor,
    backlog: readonly ZLinkActorHandoffPacket[]
  ): Promise<readonly ZLinkActorHandoffResult[]> {
    return await replayActorHandoffBacklog(
      backlog,
      (parts, returnResponse, remoteBoundSessionTarget, fallbackActorRef) =>
        this.dispatchActorPacketDirect(
          activation,
          actor.context.actorId,
          parts,
          returnResponse,
          remoteBoundSessionTarget,
          fallbackActorRef
        ),
      (index) => this.options.runtimeEventPublisher?.publish({
        sourceName: 'zlink.framework.actor-handoff',
        timestamp: new Date(),
        marker: 'backlog_enqueued',
        actorId: actor.context.actorId,
        index
      })
    );
  }
}

function requireBackendValueFactory<T>(factory: (() => T) | undefined, valueName: string): () => T {
  return () => {
    if (factory === undefined) {
      throw new Error(`Native Spot dispatch requires a backend ${valueName} factory.`);
    }
    return factory();
  };
}
