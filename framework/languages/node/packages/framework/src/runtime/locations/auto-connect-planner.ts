import type { RoutingId } from '../../contracts/Common';
import {
  ZLinkLocationAutoConnectType,
  ZLinkLocationRole,
  type ZLinkPeerLocation
} from './internal-location-contracts';
import {
  zlinkLocationAutoConnectTypeName,
  zlinkLocationRoleName
} from './canonical-codec';
import { ZLinkLocationKeyCodec } from './key-codec';
import { routingIdsEqual } from '../routing-id';
import type {
  ZLinkAutoConnectLocal,
  ZLinkAutoConnectTarget
} from './auto-connect-types';
import { routeMeshConnectionNotRequired } from '../foundation/route-mesh-connection-policy';

const encodeRoutingIdHex = ZLinkLocationKeyCodec.encodeRoutingIdHex;

export const ZLinkAutoConnectPlanner = Object.freeze({
  isRoleAllowed(type: ZLinkLocationAutoConnectType, role: ZLinkLocationRole): boolean {
    switch (type) {
      case ZLinkLocationAutoConnectType.RouteMesh:
        return role === ZLinkLocationRole.Router;
      case ZLinkLocationAutoConnectType.Fanout:
        return role === ZLinkLocationRole.Pub || role === ZLinkLocationRole.Sub;
      default:
        return false;
    }
  },

  computeDesired(
    local: ZLinkAutoConnectLocal,
    peers: readonly ZLinkPeerLocation[],
    includeDraining = false
  ): ReadonlyMap<string, ZLinkAutoConnectTarget> {
    const desired = new Map<string, ZLinkAutoConnectTarget>();
    for (const peer of peers) {
      if (!isCandidate(local, peer, includeDraining)) {
        continue;
      }

      if (!shouldDialAutoConnectPeer(local, peer)) continue;

      const target = targetOf(peer);
      desired.set(target.targetKey, target);
    }
    return desired;
  },

  computeCandidates(
    local: ZLinkAutoConnectLocal,
    peers: readonly ZLinkPeerLocation[]
  ): ReadonlyMap<string, ZLinkAutoConnectTarget> {
    const candidates = new Map<string, ZLinkAutoConnectTarget>();
    for (const peer of peers) {
      if (!isCandidate(local, peer, true)) continue;
      const target = targetOf(peer);
      candidates.set(target.targetKey, target);
    }
    return candidates;
  },

  targetKeyOf(peer: ZLinkPeerLocation): string {
    return autoConnectTargetKeyOf(peer);
  },

  computeNotRequired(
    local: ZLinkAutoConnectLocal,
    peers: readonly ZLinkPeerLocation[]
  ): readonly ZLinkAutoConnectTarget[] {
    if (local.autoConnectType !== ZLinkLocationAutoConnectType.RouteMesh) {
      return [];
    }
    return peers
      .filter(peer =>
        peer.autoConnectType === local.autoConnectType
        && peer.meshName === local.meshName
        && peer.endpoint.length > 0
        && !isAutoConnectSelf(local, peer)
        && routeMeshConnectionNotRequired(
          local.objectRole ?? 'none',
          local.hasRouteMeshServerChannel ?? false,
          peer.metadata?.objectRole === 'client'
            ? 'client'
            : peer.metadata?.objectRole === 'server' ? 'server' : 'none',
          peer.metadata?.hasRouteMeshServerChannel === 'true'
        ))
      .map(peer => ({
        targetKey: autoConnectTargetKeyOf(peer),
        nodeRid: peer.nodeRid,
        lifecycleGeneration: peer.generation,
        role: peer.role,
        endpoint: peer.endpoint,
        metadata: peer.metadata,
        ownerId: peer.ownerId,
        descriptorRevision: parseDescriptorRevision(peer.metadata?.descriptorRevision)
      }));
  }
});

export function formatAutoConnectDecision(local: ZLinkAutoConnectLocal, peer: ZLinkPeerLocation): string {
  if (peer.autoConnectType !== local.autoConnectType) {
    return `skip:type=${zlinkLocationAutoConnectTypeName(peer.autoConnectType)}`;
  }
  if (peer.meshName !== local.meshName) {
    return `skip:mesh=${peer.meshName}`;
  }
  if (!ZLinkAutoConnectPlanner.isRoleAllowed(peer.autoConnectType, peer.role)) {
    return `skip:role=${zlinkLocationRoleName(peer.role)}`;
  }
  if (peer.endpoint.length === 0) {
    return 'skip:empty-endpoint';
  }
  if (peer.draining) {
    return 'skip:draining';
  }
  if (isAutoConnectSelf(local, peer)) {
    return 'skip:self';
  }
  if (!shouldDialAutoConnectPeer(local, peer)) {
    return `skip:not-initiator localRid=${formatAutoConnectRid(local.nodeRid)}`;
  }
  return 'dial';
}

export function formatAutoConnectRid(rid: RoutingId | undefined): string {
  return rid === undefined ? '<none>' : encodeRoutingIdHex(rid);
}

function autoConnectTargetKeyOf(peer: ZLinkPeerLocation): string {
  const identity = peer.nodeRid === undefined ? peer.endpoint : encodeRoutingIdHex(peer.nodeRid);
  return `${zlinkLocationRoleName(peer.role)}|${identity}|${peer.generation}`;
}

function isCandidate(
  local: ZLinkAutoConnectLocal,
  peer: ZLinkPeerLocation,
  includeDraining: boolean
): boolean {
  return peer.autoConnectType === local.autoConnectType
    && peer.meshName === local.meshName
    && (includeDraining || !peer.draining)
    && ZLinkAutoConnectPlanner.isRoleAllowed(peer.autoConnectType, peer.role)
    && peer.endpoint.length > 0
    && !isAutoConnectSelf(local, peer);
}

function targetOf(peer: ZLinkPeerLocation): ZLinkAutoConnectTarget {
  return {
    targetKey: autoConnectTargetKeyOf(peer),
    nodeRid: peer.nodeRid,
    lifecycleGeneration: peer.generation,
    role: peer.role,
    endpoint: peer.endpoint,
    metadata: peer.metadata,
    ownerId: peer.ownerId
  };
}

function isAutoConnectSelf(local: ZLinkAutoConnectLocal, peer: ZLinkPeerLocation): boolean {
  if (local.nodeRid !== undefined && peer.nodeRid !== undefined && routingIdsEqual(local.nodeRid, peer.nodeRid)) {
    return true;
  }
  return peer.endpoint === local.endpoint;
}

function shouldDialAutoConnectPeer(local: ZLinkAutoConnectLocal, peer: ZLinkPeerLocation): boolean {
  switch (local.autoConnectType) {
    case ZLinkLocationAutoConnectType.RouteMesh:
      return local.role === ZLinkLocationRole.Router
        && peer.role === ZLinkLocationRole.Router
        && !routeMeshConnectionNotRequired(
          local.objectRole ?? 'none',
          local.hasRouteMeshServerChannel ?? false,
          peer.metadata?.objectRole === 'client'
            ? 'client'
            : peer.metadata?.objectRole === 'server' ? 'server' : 'none',
          peer.metadata?.hasRouteMeshServerChannel === 'true'
        )
        && localIsPairwiseInitiator(local, peer);
    case ZLinkLocationAutoConnectType.Fanout:
      return local.role === ZLinkLocationRole.Sub && peer.role === ZLinkLocationRole.Pub;
    default:
      return false;
  }
}

function parseDescriptorRevision(value: string | undefined): bigint | undefined {
  if (value === undefined) return undefined;
  try {
    const revision = BigInt(value);
    return revision > 0n ? revision : undefined;
  } catch {
    return undefined;
  }
}

function localIsPairwiseInitiator(local: ZLinkAutoConnectLocal, peer: ZLinkPeerLocation): boolean {
  if (local.endpoint.length === 0) {
    return true;
  }
  if (local.nodeRid !== undefined && peer.nodeRid !== undefined) {
    const byRid = encodeRoutingIdHex(local.nodeRid).localeCompare(encodeRoutingIdHex(peer.nodeRid));
    if (byRid !== 0) {
      return byRid < 0;
    }
  }
  return local.endpoint.localeCompare(peer.endpoint) < 0;
}
