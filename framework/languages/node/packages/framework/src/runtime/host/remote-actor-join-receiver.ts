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
import type { ZLinkAuthorityStore } from '../locations/internal-store-contracts';
import { encodeAuthorityKey } from '../locations/authority-key-codec';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import {
  normalizeRoutingId,
  routingIdWireHex,
  routingIdsEqual
} from '../routing-id';

export interface ZLinkCanonicalActorJoinAuthorityFence {
  readonly actorId: string;
  readonly actorNodeRid: RoutingId;
  readonly actorGeneration: bigint;
  readonly actorNodeGeneration: bigint;
  readonly expectedAuthorityOwnerGeneration: bigint;
  readonly expectedOwnerLeaseGeneration: bigint;
}

export interface ZLinkRemoteActorJoinReceiverOptions {
  readonly actorManager: () => DefaultZLinkActorManager | undefined;
  readonly spotManager: () => DefaultZLinkSpotManager | undefined;
  readonly authorityStore: () => ZLinkAuthorityStore | undefined;
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
    let join: ReturnType<typeof decodeRemoteActorJoinPayload>;
    try {
      join = decodeRemoteActorJoinPayload(payload);
    } catch (error) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestProtocolError,
        'Remote Actor Join payload is invalid.',
        error
      );
    }
    const actorManager = this.requireActorManager();
    const stableType = hasAuthorityFence(join)
      ? await this.resolveStableType(join)
      : join.actorType;
    if (hasAuthorityFence(join) && join.actorType !== stableType) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorTypeMismatch,
        `Actor '${join.actorId}' join stable type does not match its Authority row.`
      );
    }
    const actor = await actorManager.getOrCreateActor(join.actorId, stableType);
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
        },
        undefined,
        undefined,
        join.requestContentType
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

  /**
   * Canonical command 28 carries no stable type or bound-session coordinates.
   * The raw-mesh dispatcher invokes this before its normal typed Spot admission
   * so the canonical path resolves the S1 Location Store fence/type and factory
   * registration without materializing or claiming the Actor. The Spot
   * admission registry owns the temporary queue until command 40 restores the
   * hidden Actor.
   */
  async prepareCanonicalActorJoin(
    join: ZLinkCanonicalActorJoinAuthorityFence
  ): Promise<{ readonly actorType: string }> {
    const actorManager = this.requireActorManager();
    const stableType = await this.resolveStableType(join);
    actorManager.requireRelocationActorFactory(stableType);
    return { actorType: stableType };
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

  /**
   * Spec 51 §9: the Authority row, not the legacy JSON actorType field, is
   * the type source for receiver-side Actor Join admission.
   */
  private async resolveStableType(join: JoinAuthorityFence): Promise<string> {
    const store = this.options.authorityStore();
    if (store === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RelocationTargetUnavailable,
        `Actor '${join.actorId}' Join cannot read its Authority row because Location Store is unavailable.`
      );
    }
    let authority;
    try {
      authority = await store.readAuthority(encodeAuthorityKey('actor', join.actorId));
    } catch (error) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RelocationTargetUnavailable,
        `Actor '${join.actorId}' Join could not read its Authority row.`,
        error
      );
    }
    if (authority.kind === 'missing') {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `Actor '${join.actorId}' has no Authority row for Join admission.`
      );
    }

    const fence = decodeJoinAuthorityFence(join);
    if (
      authority.allocation.state !== 'active'
      || authority.allocation.objectKind !== 'actor'
      || authority.objectGeneration !== fence.objectGeneration
      || !routingIdsEqual(authority.allocation.descriptor.rid, join.actorNodeRid)
      || authority.allocation.descriptorLifecycleGeneration !== fence.nodeGeneration
      || authority.authorityOwnerGeneration !== fence.authorityOwnerGeneration
      || authority.ownerLeaseGeneration !== fence.ownerLeaseGeneration
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestProtocolError,
        `Actor '${join.actorId}' Join Authority row does not exactly match its route fence.`
      );
    }
    return authority.allocation.stableType;
  }
}

type JoinAuthorityFence = ZLinkCanonicalActorJoinAuthorityFence | {
  readonly actorId: string;
  readonly actorNodeRid: RoutingId;
  readonly actorGeneration: string;
  readonly actorNodeGeneration: string;
  readonly expectedAuthorityOwnerGeneration: string;
  readonly expectedOwnerLeaseGeneration: string;
};

function hasAuthorityFence(
  join: ReturnType<typeof decodeRemoteActorJoinPayload>
): join is ReturnType<typeof decodeRemoteActorJoinPayload> & {
  readonly actorNodeGeneration: string;
  readonly expectedAuthorityOwnerGeneration: string;
  readonly expectedOwnerLeaseGeneration: string;
} {
  return join.actorNodeGeneration !== undefined
    && join.expectedAuthorityOwnerGeneration !== undefined
    && join.expectedOwnerLeaseGeneration !== undefined;
}

function decodeJoinAuthorityFence(join: JoinAuthorityFence): {
  readonly objectGeneration: bigint;
  readonly nodeGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly ownerLeaseGeneration: bigint;
} {
  let objectGeneration: bigint;
  let nodeGeneration: bigint;
  let authorityOwnerGeneration: bigint;
  let ownerLeaseGeneration: bigint;
  try {
    objectGeneration = BigInt(join.actorGeneration);
    nodeGeneration = BigInt(join.actorNodeGeneration);
    authorityOwnerGeneration = BigInt(join.expectedAuthorityOwnerGeneration);
    ownerLeaseGeneration = BigInt(join.expectedOwnerLeaseGeneration);
  } catch (error) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RequestProtocolError,
      `Actor '${join.actorId}' Join carries an invalid Authority fence.`,
      error
    );
  }
  // object/owner generations are bounded counters; the MeshNode lifecycle is
  // an opaque full-range token, for which only zero denotes absence.
  if (
    objectGeneration <= 0n
    || nodeGeneration === 0n
    || authorityOwnerGeneration <= 0n
    || ownerLeaseGeneration <= 0n
  ) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RequestProtocolError,
      `Actor '${join.actorId}' Join carries an incomplete Authority fence.`
    );
  }
  return {
    objectGeneration,
    nodeGeneration,
    authorityOwnerGeneration,
    ownerLeaseGeneration
  };
}
