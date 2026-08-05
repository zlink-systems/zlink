import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type { RoutingId, ZLinkRouteMessageContext } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendActorRef } from '../backend';
import type { DefaultZLinkActorManager } from '../actors';
import {
  mergeRemoteBoundSessionTarget,
  preferredRemoteBoundSessionTarget
} from '../actors/actor-runtime-state';
import { decodeRemoteActorJoinPayload } from '../actors/actor-remote-wire';
import type { DefaultZLinkSpotManager } from '../spots';
import { normalizeRoutingId, routingIdWireHex } from '../routing-id';

export interface ZLinkRemoteActorJoinReceiverOptions {
  readonly actorManager: () => DefaultZLinkActorManager | undefined;
  readonly spotManager: () => DefaultZLinkSpotManager | undefined;
}

export class ZLinkRemoteActorJoinReceiver {
  constructor(private readonly options: ZLinkRemoteActorJoinReceiverOptions) {}

  async receive(
    payload: unknown,
    routeContext: ZLinkRouteMessageContext
  ): Promise<{
    readonly accepted: boolean;
    readonly actorNodeRid: string;
    readonly actorNodeRidHex?: string;
    readonly actorId: string;
    readonly actorGeneration: string;
    readonly reply?: string;
  }> {
    const join = decodeRemoteActorJoinPayload(payload);
    const actorManager = this.requireActorManager();
    const actor = await actorManager.getOrCreateActor(join.actorId, join.actorType);
    const state = actorManager.getState(join.actorId);
    if (state === undefined) {
      throw new Error(`Actor '${join.actorId}' state was not created.`);
    }
    const actorRef = {
      nodeRid: join.actorNodeRid,
      actorId: join.actorId,
      generation: BigInt(join.actorGeneration)
    };
    state.setNativeActorRef(actorRef as unknown as ZLinkBackendActorRef);
    const refreshedTarget = mergeRemoteBoundSessionTarget({
      routerChannelId: join.boundSessionRouterChannelId ?? join.routerChannelId ?? routeContext.channelName ?? '',
      targetNodeRid: join.boundSessionTargetNodeRid ?? normalizeRoutingId(routeContext.sourceNodeRid),
      spotId: join.boundSessionSpotId ?? join.sourceSpotId ?? normalizeRoutingId(routeContext.sourceNodeRid)
    }, preferredRemoteBoundSessionTarget(
      state.remoteBoundSessionTarget,
      state.boundSessionTransferTarget
    ));
    state.setRemoteBoundSessionTarget(refreshedTarget);
    const request = RuntimeMessage.from(Buffer.from(join.request, 'base64'));
    try {
      const response = await this.requireSpotManager().admitActorJoin(
        join.spotId as RoutingId,
        actor,
        request,
        (spot) => {
          state.setJoinedSpot(join.spotId as RoutingId, spot);
          return () => state.clearJoinedSpot();
        }
      );
      const reply = response.reply as Message | undefined;
      return {
        accepted: response.accepted,
        actorNodeRid: String(join.actorNodeRid),
        actorNodeRidHex: routingIdWireHex(join.actorNodeRid),
        actorId: join.actorId,
        actorGeneration: join.actorGeneration,
        reply: reply?.data().toString('base64')
      };
    } finally {
      request.close();
    }
  }

  private requireActorManager(): DefaultZLinkActorManager {
    const manager = this.options.actorManager();
    if (manager === undefined) {
      throw new Error('Actor manager runtime is not started.');
    }
    return manager;
  }

  private requireSpotManager(): DefaultZLinkSpotManager {
    const manager = this.options.spotManager();
    if (manager === undefined) {
      throw new Error('SPOT manager runtime is not started.');
    }
    return manager;
  }
}
