import { currentApplicationJobOrigin } from '../application-jobs/application-job-queue-scope';
import type { ApplicationJobOrigin } from '../application-jobs/contracts';
import type { ActorRef, ZLinkActor } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkMessageFollowOrigin } from '../foundation/service-runtime-contracts';
import type { ZLinkBackendSpot } from '../backend/contracts';
import {
  type ZLinkActorHandoffPrefixAdmission,
  type ZLinkActorHandoffPrefixRecord,
  type ZLinkRemoteBoundSessionTarget
} from '../actors';
import { routingIdsEqual } from '../routing-id';
import type { ZLinkSpotActivationLifecycleOptions } from './spot-activation';
import { ZLinkSpotActivation } from './spot-activation-state';
import { ZLinkSpotActorJoinDispatch } from './spot-actor-join-dispatch';
import type { ZLinkActorPacketExecution } from './spot-actor-packet-dispatch';
import { ZLinkSpotActorPacketDispatch } from './spot-actor-packet-dispatch';
import type { ZLinkSpotSerialTurnExecutor } from './spot-serial-turn-executor';

type AdmissionOptions = Pick<
  ZLinkSpotActivationLifecycleOptions,
  | 'actorHandoffRuntime'
  | 'admission'
  | 'actorResolver'
  | 'actorIngressMembership'
  | 'boundSessionRuntime'
  | 'detachedTaskRunner'
  | 'dispatchErrors'
  | 'locationClaim'
  | 'messageSerializers'
  | 'nativeSpotNodeProvider'
  | 'createTopicMessage'
  | 'providerResolver'
  | 'routeToActorJoinPrewarm'
>;

export class ZLinkSpotActorAdmissionCoordinator {
  constructor(private readonly options: AdmissionOptions) {}

  async dispatchActorPacket(
    origin: ApplicationJobOrigin,
    activation: ZLinkSpotActivation,
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: (response: unknown) => Promise<void> | void,
    messageFollowOrigin?: ZLinkMessageFollowOrigin,
    preparation?: import('../execution/serial-execution-queue').ZLinkSerialWorkPreparation
  ): Promise<unknown> {
    return dispatchActorPacketInMailbox<ZLinkSpotSerialTurnExecutor>(
      (operation) => activation.executeActor(actorId, operation, undefined, operation, preparation),
      (serial) =>
        this.actorPacketDispatch(activation).admitDelivery(
          serial,
          {
            origin,
            actorId,
            parts,
            returnResponse,
            remoteBoundSessionTarget,
            fallbackActorRef,
            requestTerminal,
            messageFollowOrigin
          },
          this.options.actorHandoffRuntime
        )
    );
  }

  attachNativeActorJoinDispatch(
    activation: ZLinkSpotActivation,
    nativeSpot: ZLinkBackendSpot | undefined
  ): ZLinkSpotActorJoinDispatch | undefined {
    if (nativeSpot === undefined) return undefined;
    const nativeDispatch = new ZLinkSpotActorJoinDispatch({
      nativeSpot,
      createTopicMessage: requireBackendValueFactory(
        this.options.createTopicMessage,
        'TopicMessage'
      ),
      serial: activation.serial,
      actors: {
        resolveActor: (actorId) => this.resolveLifecycleActor(activation, actorId),
        getTarget: () => activation.spot,
        defaultAccept: false,
        commitActorDeparture: (actorId) => activation.commitActorDeparture(actorId)
      },
      packets: {
        handle: (delivery) =>
          this.dispatchActorPacket(
            'remote',
            activation,
            delivery.actorId,
            delivery.parts,
            delivery.returnResponse,
            delivery.remoteBoundSessionTarget,
            delivery.fallbackActorRef,
            delivery.requestTerminal,
            delivery.messageFollowOrigin
          ),
        bindRemoteSession: (actor, sourceNodeRid, sourceSessionRid) => {
          const node = this.options.nativeSpotNodeProvider?.(activation.meshName);
          if (node === undefined || routingIdsEqual(sourceNodeRid, node.routingId)) return;
          node.bindRemoteActorSession(actor, sourceNodeRid, sourceSessionRid);
        },
        replyNoBind: (info, parts, result) =>
          this.options
            .nativeSpotNodeProvider?.(activation.meshName)
            ?.replyActorNoBind(info, parts, result)
      },
      boundSessionRuntime: this.options.boundSessionRuntime,
      messageSerializers: this.options.messageSerializers,
      providerResolver: this.options.providerResolver,
      dispatchErrors: this.options.dispatchErrors,
      claimApplicationWork:
        this.options.admission === undefined
          ? undefined
          : () => this.options.admission!.claim(activation.meshName, 'Spot route dispatch'),
      detachedTaskRunner: this.options.detachedTaskRunner
    });
    nativeDispatch.attach();
    return nativeDispatch;
  }

  /** @internal Dispatches a coordinator-owned replay without recapturing it. */
  dispatchActorPacketDirect(
    origin: ApplicationJobOrigin,
    activation: ZLinkSpotActivation,
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: (response: unknown) => Promise<void> | void
  ): Promise<unknown> {
    return activation.executeActor(actorId, (actorSerial) =>
      this.dispatchActorPacketInActorTurn(
        origin,
        activation,
        actorSerial,
        actorId,
        parts,
        returnResponse,
        remoteBoundSessionTarget,
        fallbackActorRef,
        requestTerminal
      )
    );
  }

  /**
   * Atomically restores one durable packet prefix at the Actor FIFO tail.
   * The prefix itself is not an executing Actor record. Its child callbacks
   * enter the Actor context one by one after the prefix reaches the head.
   *
   * @internal
   */
  admitActorPacketPrefix(
    activation: ZLinkSpotActivation,
    actorId: string,
    records: readonly ZLinkActorHandoffPrefixRecord[]
  ): ZLinkActorHandoffPrefixAdmission {
    return activation.admitActorPacketPrefix(
      actorId,
      records,
      (actorSerial, parts, returnResponse, remoteBoundSessionTarget, fallbackActorRef) =>
        this.dispatchActorPacketInActorTurn(
          currentApplicationJobOrigin(),
          activation,
          actorSerial,
          actorId,
          parts,
          returnResponse,
          remoteBoundSessionTarget,
          fallbackActorRef
        )
    );
  }

  private dispatchActorPacketInActorTurn(
    origin: ApplicationJobOrigin,
    activation: ZLinkSpotActivation,
    actorSerial: ZLinkSpotSerialTurnExecutor,
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: (response: unknown) => Promise<void> | void
  ): Promise<unknown> {
    return this.actorPacketDispatch(activation).dispatch(
      actorSerial,
      origin,
      actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef,
      requestTerminal
    );
  }

  private actorPacketDispatch(activation: ZLinkSpotActivation): ZLinkSpotActorPacketDispatch {
    if (activation.actorPacketDispatch !== undefined) return activation.actorPacketDispatch;
    activation.actorPacketDispatch = new ZLinkSpotActorPacketDispatch({
      admission: this.options.admission,
      spot: activation.spot,
      spotId: () => String(activation.spotId),
      registry: activation.actorHandlers,
      resolveActor: (targetActorId) =>
        this.options.actorResolver === undefined
          ? activation.resolveJoinedActor(targetActorId)
          : this.options.actorResolver(targetActorId),
      membershipSpotId: activation.spotId,
      membership: this.options.actorIngressMembership,
      onDisconnectActor: (actor) =>
        activation.serial.execute(() => activation.spot.onDisconnectActor?.(actor)),
      actorResponseSender: this.options.boundSessionRuntime?.sendActorResponse.bind(
        this.options.boundSessionRuntime
      ),
      actorErrorSender: this.options.boundSessionRuntime?.sendActorError.bind(
        this.options.boundSessionRuntime
      ),
      providerResolver: this.options.providerResolver,
      messageSerializers: this.options.messageSerializers,
      dispatchErrors: this.options.dispatchErrors,
      routeToActorJoinPrewarm: this.options.routeToActorJoinPrewarm
    });
    return activation.actorPacketDispatch;
  }

  /** Resolves native lifecycle records; messages use resolveActorIngress. */
  private resolveLifecycleActor(
    activation: ZLinkSpotActivation,
    actorId: string
  ): ZLinkActor | undefined {
    if (activation.hasDepartedActor(actorId)) return undefined;
    // The manager owns relocation visibility. Membership alone cannot publish
    // a staged Actor; resolver absence supports standalone Spot runtimes.
    return this.options.actorResolver === undefined
      ? activation.resolveJoinedActor(actorId)
      : this.options.actorResolver(actorId);
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

/** Releases the Actor FIFO before awaiting a transferred admission terminal. */
export async function dispatchActorPacketInMailbox<TSerial>(
  enqueue: (
    operation: (serial: TSerial) => Promise<ZLinkActorPacketExecution>
  ) => Promise<ZLinkActorPacketExecution>,
  dispatch: (serial: TSerial) => Promise<ZLinkActorPacketExecution>
): Promise<unknown> {
  const result = await enqueue(dispatch);
  return 'terminal' in result ? await result.terminal : result.response;
}
