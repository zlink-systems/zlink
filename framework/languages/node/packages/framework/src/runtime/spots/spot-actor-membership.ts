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
import {
  ZLinkConfigurationException
} from '../configuration';
import { ZLinkDispatchErrorReporter } from '../channels';
import {
  ZLINK_ACTOR_JOIN_ENTRY_SPOT_RUNTIME,
  ZLinkSpotActorDispatcher
} from '../actors';
import {
  encodeFrameworkPayloadMessage
} from '../messaging/payload-codec';
import { routingIdsEqual } from '../routing-id';
import type { ZLinkSpotActivation } from './spot-activation-state';
import type { ZLinkSpotActorTransferRuntime } from './spot-runtime-ports';
import type { ZLinkSpotRouteResolver } from './spot-routing-internal';

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

export type ZLinkActorJoinRollback = () => Promise<void> | void;

export class ZLinkSpotActorMembership {
  constructor(private readonly options: ZLinkSpotActorMembershipOptions) {}

  async admitActorJoin(
    spotId: RoutingId,
    actor: ZLinkActor,
    request: Message,
    commit: (spot: ZLinkSpot) => Promise<ZLinkActorJoinRollback | void> | ZLinkActorJoinRollback | void,
    signal?: AbortSignal,
    leaveSource?: () => Promise<void>
  ): Promise<ZLinkSpotActorJoinResult> {
    throwIfAborted(signal);
    const activation = this.requireActivation(spotId);
    const dispatcher = this.createActorDispatcher(activation);
    const transaction: {
      rollbackExternal?: ZLinkActorJoinRollback;
      rollbackMembership?: () => void;
      committed: boolean;
    } = { committed: false };
    let response: ZLinkSpotActorJoinResult;
    try {
      response = await dispatcher.evaluateActorJoin(actor, request);
      if (response.accepted) {
        if (leaveSource === undefined) {
          await this.options.entrySpotCallbacks?.onLeaveActor(actor, signal);
        } else {
          await leaveSource();
        }
        await dispatcher.commitActorJoin(actor, async () => {
          const rollback = await commit(activation.spot);
          if (rollback !== undefined) transaction.rollbackExternal = rollback;
          transaction.rollbackMembership = activation.commitActorJoin(actor);
          transaction.committed = true;
        });
      }
    } catch (error) {
      if (transaction.committed) {
        transaction.rollbackMembership?.();
        await transaction.rollbackExternal?.();
      }
      throw error;
    }
    return {
      accepted: response.accepted,
      reply: response.reply === undefined
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
    const localEntryNodeRid =
      (meshName === undefined ? undefined : this.options.nodeRidProvider?.(meshName)) ??
      this.options.entryNodeRidProvider?.() ??
      this.options.entryNodeRid ??
      this.options.nodeRid;
    const entrySpotId = meshName === undefined
      ? undefined
      : this.options.entrySpotIdProvider?.(meshName);
    const resolvedEntry = entrySpotId === undefined
      ? undefined
      : await this.options.spotRouteResolver?.resolve(entrySpotId, signal);
    const entryNodeRid = resolvedEntry?.targetNodeRid ??
      this.options.actorTransferRuntime?.actorEntryNodeRid(actor) ??
      localEntryNodeRid;
    if (entryNodeRid === undefined) {
      throw new ZLinkConfigurationException('Spot actor leave requires an Entry Spot node routing id.');
    }
    const remoteEntry = localEntryNodeRid !== undefined && !routingIdsEqual(entryNodeRid, localEntryNodeRid);
    if (!remoteEntry) {
      await activation.serial.execute(async () => {
        activation.beginActorTransfer(actor.context.actorId);
        await activation.spot.onLeaveActor(actor);
        activation.commitActorDeparture(actor.context.actorId);
        this.options.actorTransferRuntime?.clearRoutedActor(actor);
      });
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
      await context[ZLINK_ACTOR_JOIN_ENTRY_SPOT_RUNTIME](
        entryNodeRid,
        ZLinkMessage.fromEncoded(ZLinkEncodedPayload.from(request.data())),
        signal
      );
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
    await activation.serial.execute(async () => {
      activation.beginActorTransfer(actor.context.actorId);
      await activation.spot.onLeaveActor(actor);
      activation.commitActorDeparture(actor.context.actorId);
    });
  }


  async prepareActorLeaveForTransfer(
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    throwIfAborted(signal);
    const activation = this.requireActivation(spotId);
    await activation.serial.execute(() => activation.spot.onLeaveActor(actor));
  }

  async commitActorLeaveAfterTransfer(spotId: RoutingId, actorId: string): Promise<void> {
    const activation = this.requireActivation(spotId);
    await activation.serial.execute(() => activation.commitActorDeparture(actorId));
  }

  async restoreActorAfterFailedTransfer(
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    throwIfAborted(signal);
    const activation = this.requireActivation(spotId);
    await activation.serial.execute(async () => {
      activation.cancelActorTransfer(actor.context.actorId);
      await activation.spot.onJoinedActor(actor);
    });
  }

  async beginActorTransfer(spotId: RoutingId, actorId: string): Promise<void> {
    const activation = this.requireActivation(spotId);
    await activation.serial.execute(() => {
      activation.beginActorTransfer(actorId);
    });
  }

  async cancelActorTransfer(spotId: RoutingId, actorId: string): Promise<void> {
    const activation = this.requireActivation(spotId);
    await activation.serial.execute(() => {
      activation.cancelActorTransfer(actorId);
    });
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
    await activation.serial.execute(() =>
      activation.spot.onDisconnectActor?.(joinedActor));
    return true;
  }

  private requireActivation(spotId: RoutingId, meshName?: string): ZLinkSpotActivation {
    const activation = this.options.resolveActivation(spotId, meshName);
    if (activation === undefined) {
      throw new ZLinkConfigurationException(`Spot '${spotId}' is not active.`);
    }
    return activation;
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
