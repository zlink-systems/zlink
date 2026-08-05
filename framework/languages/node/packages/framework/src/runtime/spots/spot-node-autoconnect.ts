import type { ZLinkLocationOptionOverrides } from '../../contracts/Locations/Options';
import type { ZLinkPeerLocationFilter } from '../../contracts/Locations/Keys';
import type { ZLinkPeerLocationResolver } from '../../contracts/Locations/Resolvers';
import type { ZLinkDomainLocationStore as ZLinkLocationStore } from '../locations/domain-store-contract';
import type { ZLinkPeerLocation } from '../../contracts/Locations/Rows';
import { ZLinkLocationAutoConnectType } from '../../contracts/Locations/Values';
import {
  ZLinkFrameworkRuntimeState,
  ZLinkLocationRole
} from '../../contracts';
import type { ZLinkSpotNodeOptions } from '../configuration';
import type { ZLinkBackendMeshNode } from '../backend/contracts';
import { toBackendRoutingId as toBackendRoutingId } from '../routing-id';
import {
  ZLinkLocationRuntime,
  ZLinkOwnerLeaseTracker,
  type IZLinkAutoConnectExecutor,
  type ZLinkAutoConnectLocal,
  type ZLinkAutoConnectTarget,
  type ZLinkLocationEventSink,
  type ZLinkLocationRuntimeStores
} from '../locations';

export interface ZLinkSpotNodeLocationAutoConnectContext {
  readonly runtime: ZLinkLocationRuntime;
  readonly stores: ZLinkLocationRuntimeStores;
  readonly options: ZLinkLocationOptionOverrides;
  readonly leaseTracker: ZLinkOwnerLeaseTracker;
  readonly resolver: ZLinkPeerLocationResolver;
  readonly events?: ZLinkLocationEventSink;
  readonly changeStampStore?: Pick<ZLinkLocationStore, 'getMeshNodeChangeStamp'>;
}

export interface ZLinkSpotNodeAutoConnectCapability {
  readonly local: ZLinkAutoConnectLocal;
  readonly localRow?: ZLinkPeerLocation;
  readonly executor: IZLinkAutoConnectExecutor;
}

export function createSpotNodeLocationAutoConnectContext(
  runtime: ZLinkLocationRuntime,
  stores: ZLinkLocationRuntimeStores,
  options: ZLinkLocationOptionOverrides,
  events?: ZLinkLocationEventSink
): ZLinkSpotNodeLocationAutoConnectContext {
  const leaseTracker = new ZLinkOwnerLeaseTracker({
    store: stores.ownerLeaseStore,
    options
  });
  return {
    runtime,
    stores,
    options,
    leaseTracker,
    resolver: meshDescriptorPeerResolver(runtime),
    events,
    // The opaque provider has no cross-process change notification primitive.
    // A process-local inherited stamp would hide descriptors written by peers,
    // so RouteMesh discovery polls the authoritative descriptor scan.
    changeStampStore: undefined
  };
}

export function spotNodeAutoConnectCapability(
  spotNodeName: string,
  spotNode: ZLinkSpotNodeOptions,
  node: ZLinkBackendMeshNode
): ZLinkSpotNodeAutoConnectCapability | undefined {
  if (spotNode.router === undefined) {
    return undefined;
  }
  // The node has already started when this capability is built. Publishing
  // the configured bind string would advertise port 0 for wildcard binds,
  // so peers must receive the concrete endpoint resolved by Core.
  const status = node.status();
  const endpoint = status.localEndpoint;
  const local: ZLinkAutoConnectLocal = {
    autoConnectType: ZLinkLocationAutoConnectType.RouteMesh,
    meshName: spotNodeName,
    role: ZLinkLocationRole.Router,
    nodeRid: String(status.routingId),
    endpoint,
    objectRole: spotNode.objectRole ?? 'none',
    hasRouteMeshServerChannel: Object.values(spotNode.meshChannels ?? {})
      .some((channel) => channel.server === true)
  };
  const manualConnections = hasManualRouterConnections(spotNode);
  return {
    local,
    executor: new ZLinkSpotNodeAutoConnectExecutor(
      node,
      manualConnections
    )
  };
}

function meshDescriptorPeerResolver(
  runtime: ZLinkLocationRuntime
): ZLinkPeerLocationResolver {
  return {
    async listLivePeers(
      filter: ZLinkPeerLocationFilter,
      signal?: AbortSignal
    ): Promise<readonly ZLinkPeerLocation[]> {
      if (
        filter.meshName === undefined
        || (
          filter.autoConnectType !== undefined
          && filter.autoConnectType !== ZLinkLocationAutoConnectType.RouteMesh
        )
        || (
          filter.role !== undefined
          && filter.role !== ZLinkLocationRole.Router
        )
      ) {
        return [];
      }
      const descriptors = await runtime.listLiveMeshNodes(filter.meshName, signal);
      return descriptors
        .map((descriptor): ZLinkPeerLocation => ({
          autoConnectType: ZLinkLocationAutoConnectType.RouteMesh,
          meshName: descriptor.meshName,
          nodeRid: descriptor.rid,
          role: ZLinkLocationRole.Router,
          endpoint: descriptor.endpoint,
          weight: 100,
          draining: descriptor.state !== ZLinkFrameworkRuntimeState.Serving,
          value: descriptor.descriptorRevision,
          ownerId: descriptor.ownerId,
          generation: descriptor.lifecycleGeneration,
          updatedAt: descriptor.updatedAt,
          metadata: {
            objectRole: descriptor.objectRole,
            hasRouteMeshServerChannel:
              String(Object.keys(descriptor.channelWeights).length > 0),
            descriptorRevision: descriptor.descriptorRevision.toString(),
            securityIdentity: descriptor.securityIdentity
          }
        }))
        .filter(peer =>
          (filter.nodeRid === undefined || String(peer.nodeRid) === String(filter.nodeRid))
          && (filter.endpoint === undefined || peer.endpoint === filter.endpoint)
        );
    }
  };
}

function hasManualRouterConnections(spotNode: ZLinkSpotNodeOptions): boolean {
  return (spotNode.router?.manualConnections?.length ?? 0) > 0
    || (spotNode.router?.manualPeerConnections?.length ?? 0) > 0;
}

class ZLinkSpotNodeAutoConnectExecutor implements IZLinkAutoConnectExecutor {
  private readonly connectionIntents = new Map<string, bigint>();

  constructor(
    private readonly node: ZLinkBackendMeshNode,
    private readonly manualConnections: boolean
  ) {}

  connect(target: ZLinkAutoConnectTarget): boolean {
    if (this.manualConnections) return false;
    return this.connectPeer(target);
  }

  disconnect(target: ZLinkAutoConnectTarget): void {
    if (this.manualConnections) return;
    this.disconnectPeer(target);
  }

  disconnectStalePeers(targets: readonly ZLinkAutoConnectTarget[]): void {
    if (this.manualConnections) return;
    const current = new Map(targets
      .filter((target) => target.nodeRid !== undefined)
      .map((target) => [String(target.nodeRid), target]));
    const peers = this.node.peers();
    for (const peer of peers) {
      if (peer.routingId === null) continue;
      const target = current.get(String(peer.routingId));
      if (
        target !== undefined
        && peer.endpoint === target.endpoint
      ) {
        continue;
      }
      this.node.disconnectPeer(peer.routingId, peer.lifecycleGeneration);
    }
  }

  isDisconnected(target: ZLinkAutoConnectTarget): boolean {
    // A connect intent still owns transport state even when its peer has not
    // reached the admitted table yet. Shutdown must remove that intent before
    // it can treat the automatic route as disconnected.
    if (this.connectionIntents.has(connectionKey(target))) return false;
    return !this.node.peers().some((peer) =>
      peer.routingId !== null &&
      peer.endpoint === target.endpoint &&
      (target.nodeRid === undefined || String(peer.routingId) === String(target.nodeRid)) &&
      peer.lifecycleGeneration === target.lifecycleGeneration &&
      peer.state === 3);
  }

  replaceNotRequired(targets: readonly ZLinkAutoConnectTarget[]): void {
    this.node.replaceDiscoveredNotRequiredPeers?.(targets
      .filter((target): target is ZLinkAutoConnectTarget & {
        readonly nodeRid: NonNullable<ZLinkAutoConnectTarget['nodeRid']>;
      } => target.nodeRid !== undefined)
      .map(target => ({
        nodeRoutingId: String(target.nodeRid),
        lifecycleGeneration: target.lifecycleGeneration,
        descriptorRevision: target.descriptorRevision ?? 1n,
        endpoint: target.endpoint
      })));
  }

  private connectPeer(target: ZLinkAutoConnectTarget): boolean {
    const key = connectionKey(target);
    if (this.connectionIntents.has(key)) {
      return true;
    }
    try {
      const connectionIntentId = this.node.connectPeer({
        endpoint: target.endpoint,
        expectedRid: target.nodeRid === undefined
          ? undefined
          : toBackendRoutingId(target.nodeRid),
        expectedSecurityIdentity: target.metadata?.securityIdentity,
        expectedLifecycleGeneration: target.lifecycleGeneration
      });
      this.connectionIntents.set(key, connectionIntentId);
      return true;
    } catch {
      // A discovered endpoint can become unavailable between the store read and
      // the socket connect. Leave the target inactive so the next reconciliation
      // can retry without terminating the host's background runtime.
      return false;
    }
  }

  private disconnectPeer(target: ZLinkAutoConnectTarget): void {
    const key = connectionKey(target);
    const connectionIntentId = this.connectionIntents.get(key);
    if (connectionIntentId !== undefined) {
      this.node.removePeerConnection(connectionIntentId);
      this.connectionIntents.delete(key);
    }
    for (const peer of this.node.peers()) {
      if (peer.routingId === null) {
        continue;
      }
      if (peer.endpoint !== target.endpoint) {
        continue;
      }
      if (target.nodeRid !== undefined && String(peer.routingId) !== String(target.nodeRid)) {
        continue;
      }
      this.node.disconnectPeer(peer.routingId, peer.lifecycleGeneration);
    }
  }
}

function connectionKey(target: ZLinkAutoConnectTarget): string {
  return `${target.nodeRid ?? ''}\0${target.lifecycleGeneration}\0${target.endpoint}`;
}
