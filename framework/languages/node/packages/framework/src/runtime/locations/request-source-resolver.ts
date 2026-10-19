import type { RoutingId } from '../../contracts/Common';
import type { ZLinkMeshNodeDescriptor } from '../../contracts/Locations/Rows';
import type { ZLinkLocationOwnerToken } from './internal-location-contracts';

/** Source request owner values the first accepting owner records (Location runtime §9.3). */
export interface ZLinkRequestSourceFence {
  readonly ownerId: string;
  readonly leaseGeneration: bigint;
  readonly nodeRid: RoutingId;
  readonly nodeGeneration: bigint;
}

export interface ZLinkRequestSourceResolverOptions {
  readonly listLiveMeshNodes: (
    meshName: string,
    signal?: AbortSignal
  ) => Promise<readonly ZLinkMeshNodeDescriptor[]>;
  readonly remainingOwnerTokenLeaseMs: (
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ) => Promise<number>;
  readonly monotonicNowMs?: () => number;
}

interface PolledSource {
  readonly fence: ZLinkRequestSourceFence;
  readonly leaseDeadlineMs: number;
}

/**
 * Chooses the descriptor that supplies the source request owner values (Session·Actor binding
 * §4): the admitted sender's descriptor with the exact RID and node lifecycle generation whose
 * owner lease passes the Location runtime §4.1 check. Auto-connect polling keeps the live
 * descriptors here with the local lease deadline computed from the same lease read. A sender
 * missing from them is read from the Store once and kept with them.
 */
export class ZLinkRequestSourceResolver {
  private readonly polled = new Map<string, Map<RoutingId, PolledSource>>();
  private readonly monotonicNowMs: () => number;

  constructor(private readonly options: ZLinkRequestSourceResolverOptions) {
    this.monotonicNowMs = options.monotonicNowMs ?? (() => performance.now());
  }

  /** Replaces the polled descriptors of one mesh with a live descriptor listing. */
  async observeLiveDescriptors(
    meshName: string,
    descriptors: readonly ZLinkMeshNodeDescriptor[],
    signal?: AbortSignal
  ): Promise<void> {
    const sources = new Map<RoutingId, PolledSource>();
    for (const descriptor of descriptors) {
      const source = await this.polledSource(descriptor, signal);
      if (source !== undefined) sources.set(descriptor.rid, source);
    }
    this.polled.set(meshName, sources);
  }

  /**
   * Returns the recorded values, undefined when the sender has no live descriptor, or a promise
   * while the Store is read once for a sender the polled descriptors do not contain.
   */
  accept(
    meshName: string,
    nodeRid: RoutingId,
    nodeGeneration: bigint
  ): ZLinkRequestSourceFence | undefined | Promise<ZLinkRequestSourceFence | undefined> {
    const known = this.polled.get(meshName)?.get(nodeRid);
    if (known !== undefined && known.fence.nodeGeneration === nodeGeneration) {
      return this.monotonicNowMs() < known.leaseDeadlineMs ? known.fence : undefined;
    }
    return this.readOnce(meshName, nodeRid, nodeGeneration);
  }

  private async readOnce(
    meshName: string,
    nodeRid: RoutingId,
    nodeGeneration: bigint
  ): Promise<ZLinkRequestSourceFence | undefined> {
    let descriptors: readonly ZLinkMeshNodeDescriptor[];
    try {
      descriptors = await this.options.listLiveMeshNodes(meshName);
    } catch {
      // A Store read that cannot show a live descriptor leaves the request without source owner
      // values; the accepting owner ends it Unavailable (binding §4).
      return undefined;
    }
    const descriptor = descriptors.find(
      (value) => value.rid === nodeRid && value.lifecycleGeneration === nodeGeneration
    );
    const source = descriptor === undefined ? undefined : await this.polledSource(descriptor);
    if (source === undefined) return undefined;
    let sources = this.polled.get(meshName);
    if (sources === undefined) {
      sources = new Map();
      this.polled.set(meshName, sources);
    }
    sources.set(nodeRid, source);
    return source.fence;
  }

  private async polledSource(
    descriptor: ZLinkMeshNodeDescriptor,
    signal?: AbortSignal
  ): Promise<PolledSource | undefined> {
    if (
      descriptor.ownerId.length === 0 ||
      descriptor.leaseGeneration <= 0n ||
      descriptor.lifecycleGeneration <= 0n
    ) {
      return undefined;
    }
    const remainingMs = await this.options.remainingOwnerTokenLeaseMs(
      { ownerId: descriptor.ownerId, leaseGeneration: descriptor.leaseGeneration },
      signal
    );
    if (remainingMs <= 0) return undefined;
    return {
      fence: {
        ownerId: descriptor.ownerId,
        leaseGeneration: descriptor.leaseGeneration,
        nodeRid: descriptor.rid,
        nodeGeneration: descriptor.lifecycleGeneration
      },
      leaseDeadlineMs: this.monotonicNowMs() + remainingMs
    };
  }
}
