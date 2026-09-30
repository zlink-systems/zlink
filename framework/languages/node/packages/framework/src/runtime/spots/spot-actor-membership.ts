import type {
  RoutingId,
  ZLinkActor,
  ZLinkMessageSerializer,
  ZLinkSpot,
  ZLinkSpotActorJoinResult
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import { ZLinkEncodedPayload, ZLinkMessage } from '../../contracts';
import { throwIfAborted } from '../abort';
import type { Message } from '../../contracts/Common/Message';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import { ZLinkConfigurationException } from '../configuration';
import { ZLinkDispatchErrorReporter } from '../channels';
import { ZLINK_ACTOR_JOIN_ENTRY_SPOT_RUNTIME, ZLinkSpotActorDispatcher } from '../actors';
import { encodeFrameworkPayloadMessage } from '../messaging/payload-codec';
import { routingIdsEqual } from '../routing-id';
import type { ZLinkSpotActivation } from './spot-activation-state';
import type { ZLinkSpotActorTransferRuntime } from './spot-runtime-ports';
import type { ZLinkSpotRouteResolver } from './spot-routing-internal';
import {
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind,
  ZLinkRuntimeDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason
} from '../../contracts/Dispatch/ZLinkDispatchOptions';

export interface ZLinkSpotActorMembershipOptions {
  readonly resolveActivation: (
    spotId: RoutingId,
    meshName?: string
  ) => ZLinkSpotActivation | undefined;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
  readonly entrySpotCallbacks?: {
    onLeaveActor(actor: ZLinkActor, signal?: AbortSignal): Promise<void>;
  };
  readonly nodeRid?: RoutingId;
  readonly nodeRidProvider?: (meshName: string) => RoutingId | undefined;
  readonly entryNodeRid?: RoutingId;
  readonly entryNodeRidProvider?: () => RoutingId | undefined;
  readonly entrySpotIdProvider?: (meshName: string) => string | undefined;
  readonly spotRouteResolver?: ZLinkSpotRouteResolver;
  readonly actorTransferRuntime?: ZLinkSpotActorTransferRuntime;
}

export class ZLinkSpotActorMembership {
  constructor(private readonly options: ZLinkSpotActorMembershipOptions) {}

  async admitActorJoin(
    spotId: RoutingId,
    actor: ZLinkActor,
    request: Message,
    commit: (spot: ZLinkSpot) => Promise<void> | void,
    signal?: AbortSignal,
    leaveSource?: () => Promise<void>,
    contentType = 'application/json'
  ): Promise<ZLinkSpotActorJoinResult> {
    throwIfAborted(signal);
    const activation = this.requireActivation(spotId);
    return await this.runLifecycleOperation(activation, async () => {
      return await this.admitActorJoinCore(
        activation,
        actor,
        request,
        commit,
        signal,
        leaveSource,
        contentType
      );
    });
  }

  private async admitActorJoinCore(
    activation: ZLinkSpotActivation,
    actor: ZLinkActor,
    request: Message,
    commit: (spot: ZLinkSpot) => Promise<void> | void,
    signal: AbortSignal | undefined,
    leaveSource: (() => Promise<void>) | undefined,
    contentType: string
  ): Promise<ZLinkSpotActorJoinResult> {
    const dispatcher = this.createActorDispatcher(activation);
    const response = await dispatcher.evaluateActorJoin(actor, request, contentType);
    if (response.accepted) {
      await commit(activation.spot);
      activation.commitActorJoin(actor);
      try {
        await dispatcher.notifyJoinActor(actor);
      } finally {
        if (leaveSource === undefined) {
          void this.options.entrySpotCallbacks?.onLeaveActor(actor, signal).catch((error) => {
            this.options.dispatchErrors?.report({
              surface: ZLinkDispatchErrorSurface.SpotActor,
              messageKind: ZLinkDispatchMessageKind.ActorSend,
              reason: ZLinkRuntimeDispatchErrorReason.HandlerException,
              action: ZLinkRuntimeDispatchErrorAction.Drop,
              actorId: actor.context.actorId,
              error
            });
          });
        } else {
          await leaveSource();
        }
      }
    }
    return {
      accepted: response.accepted,
      reply:
        response.reply === undefined
          ? undefined
          : encodeFrameworkPayloadMessage(response.reply, this.options.messageSerializers)
    };
  }

  async leaveActor(
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal,
    meshName?: string
  ): Promise<void> {
    throwIfAborted(signal);
    const activation = this.requireActivation(spotId, meshName);
    await this.runLifecycleOperation(activation, () =>
      this.leaveActorCore(activation, actor, signal, meshName)
    );
  }

  private async leaveActorCore(
    activation: ZLinkSpotActivation,
    actor: ZLinkActor,
    signal?: AbortSignal,
    meshName?: string
  ): Promise<void> {
    const localEntryNodeRid =
      (meshName === undefined ? undefined : this.options.nodeRidProvider?.(meshName)) ??
      this.options.entryNodeRidProvider?.() ??
      this.options.entryNodeRid ??
      this.options.nodeRid;
    const entrySpotId =
      meshName === undefined ? undefined : this.options.entrySpotIdProvider?.(meshName);
    const resolvedEntry =
      entrySpotId === undefined
        ? undefined
        : await this.options.spotRouteResolver?.resolve(entrySpotId, signal);
    const entryNodeRid =
      resolvedEntry?.targetNodeRid ??
      this.options.actorTransferRuntime?.actorEntryNodeRid(actor) ??
      localEntryNodeRid;
    if (entryNodeRid === undefined) {
      throw new ZLinkConfigurationException(
        'Spot actor leave requires an Entry Spot node routing id.'
      );
    }
    const remoteEntry =
      localEntryNodeRid !== undefined && !routingIdsEqual(entryNodeRid, localEntryNodeRid);
    if (!remoteEntry) {
      const leaveSource = async () => {
        activation.beginActorTransfer(actor.context.actorId);
        await activation.spot.onLeaveActor(actor);
        activation.commitActorDeparture(actor.context.actorId);
        this.options.actorTransferRuntime?.clearRoutedActor(actor);
      };
      // A Spot actor handler already owns this activation's serial turn.
      // Complete its lifecycle transition inside that turn instead of trying
      // to acquire a second turn for the same owner.
      if (activation.serial.isCurrentTurn) {
        await leaveSource();
      } else {
        await activation.serial.execute(leaveSource);
      }
    }
    const request = RuntimeMessage.from(Buffer.alloc(0));
    try {
      const context = actor.context as typeof actor.context & {
        [ZLINK_ACTOR_JOIN_ENTRY_SPOT_RUNTIME](
          nodeRid: RoutingId | undefined,
          request: unknown,
          signal?: AbortSignal
        ): Promise<boolean>;
      };
      const joinEntry = context[ZLINK_ACTOR_JOIN_ENTRY_SPOT_RUNTIME](
        entryNodeRid,
        ZLinkMessage.fromEncoded(ZLinkEncodedPayload.from(request.data())),
        signal
      );
      // An Actor request may leave its Spot before producing its terminal
      // response.  Rejoining Entry can require control work that is queued
      // behind this Spot turn, so retain the request's turn identity but yield
      // the gate while that Framework operation is in flight.
      const turn = activation.serial.currentTurn;
      if (turn !== undefined) {
        await turn.yieldFrameworkPromise(joinEntry);
      } else {
        await joinEntry;
      }
    } finally {
      request.close();
    }
  }

  async notifyActorLeftAfterTransfer(
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    throwIfAborted(signal);
    const activation = this.requireActivation(spotId);
    await this.runLifecycleOperation(activation, () =>
      activation.serial.execute(async () => {
        activation.beginActorTransfer(actor.context.actorId);
        await activation.spot.onLeaveActor(actor);
        activation.commitActorDeparture(actor.context.actorId);
      })
    );
  }

  async prepareActorLeaveForTransfer(
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    throwIfAborted(signal);
    const activation = this.requireActivation(spotId);
    await this.runLifecycleOperation(activation, () =>
      activation.serial.execute(() => activation.spot.onLeaveActor(actor))
    );
  }

  async commitActorLeaveAfterTransfer(spotId: RoutingId, actorId: string): Promise<void> {
    const activation = this.requireActivation(spotId);
    await this.runLifecycleOperation(activation, () =>
      activation.serial.execute(() => activation.commitActorDeparture(actorId))
    );
  }

  async restoreActorAfterFailedTransfer(
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    throwIfAborted(signal);
    const activation = this.requireActivation(spotId);
    await this.runLifecycleOperation(activation, () =>
      activation.serial.execute(async () => {
        activation.cancelActorTransfer(actor.context.actorId);
        await activation.spot.onJoinedActor(actor);
      })
    );
  }

  async beginActorTransfer(spotId: RoutingId, actorId: string): Promise<void> {
    const activation = this.requireActivation(spotId);
    await this.runLifecycleOperation(activation, () =>
      activation.serial.execute(() => activation.beginActorTransfer(actorId))
    );
  }

  async cancelActorTransfer(spotId: RoutingId, actorId: string): Promise<void> {
    const activation = this.requireActivation(spotId);
    await this.runLifecycleOperation(activation, () =>
      activation.serial.execute(() => activation.cancelActorTransfer(actorId))
    );
  }

  async notifyJoinedActorDisconnected(
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<boolean> {
    throwIfAborted(signal);
    const activation = this.options.resolveActivation(spotId);
    if (activation === undefined) {
      return false;
    }
    const joinedActor = activation.resolveJoinedActor(actor.context.actorId);
    if (joinedActor === undefined) {
      return false;
    }
    await activation.serial.execute(() => activation.spot.onDisconnectActor?.(joinedActor));
    return true;
  }

  private requireActivation(spotId: RoutingId, meshName?: string): ZLinkSpotActivation {
    const activation = this.options.resolveActivation(spotId, meshName);
    if (activation === undefined) {
      throw new ZLinkConfigurationException(`Spot '${spotId}' is not active.`);
    }
    return activation;
  }

  private async runLifecycleOperation<T>(
    activation: ZLinkSpotActivation,
    operation: () => Promise<T> | T
  ): Promise<T> {
    const pending = activation.serial.executeLifecycleOperation(operation);
    return activation.serial.isCurrentTurn
      ? await activation.serial.currentTurn!.yieldFrameworkPromise(pending)
      : await pending;
  }

  private createActorDispatcher(activation: ZLinkSpotActivation): ZLinkSpotActorDispatcher {
    return new ZLinkSpotActorDispatcher({
      registry: activation.actorHandlers,
      spot: activation.spot,
      providerResolver: this.options.providerResolver,
      serial: activation.serial,
      messageSerializers: this.options.messageSerializers
    });
  }
}
