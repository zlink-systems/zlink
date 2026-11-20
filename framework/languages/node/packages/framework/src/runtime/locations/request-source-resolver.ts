import { randomBytes } from 'node:crypto';
import { ZLinkFrameworkRuntimeState } from '../../contracts';
import type { ZLinkBackendMeshNode } from '../backend/contracts';
import type { RoutingId } from '../../contracts/Common';
import type { ZLinkMeshNodeDescriptor } from '../../contracts/Locations/Rows';
import type { ZLinkLocationOwnerToken } from './internal-location-contracts';
import { encodeRoutingIdStorageHex, routingIdsEqual } from '../routing-id';
import { awaitWithAbort, throwIfAborted } from '../abort';

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
  readonly descriptor: ZLinkMeshNodeDescriptor;
  readonly fence: ZLinkRequestSourceFence;
  readonly leaseDeadlineMs: number;
}

/**
 * Chooses the descriptor that supplies the source request owner values (Session·Actor binding
 * §4): the admitted sender's descriptor with the exact RID and node lifecycle generation whose
 * owner lease passes the Location runtime §4.1 check. Auto-connect polling keeps the live
 * descriptors here with the local lease deadline computed from the same lease read. A sender
 * missing from them is read from the Store once and kept with them. The same observation owns
 * Actor and User Spot placement (MeshNode §5.2); refreshes wake callers without request polling.
 */
export class ZLinkRequestSourceResolver {
  private readonly polled = new Map<string, Map<string, PolledSource>>();
  private readonly placementChanges = new Map<string, Set<() => void>>();
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
    const sources = new Map<string, PolledSource>();
    for (const descriptor of descriptors) {
      const source = await this.polledSource(descriptor, signal);
      if (source !== undefined) sources.set(encodeRoutingIdStorageHex(descriptor.rid), source);
    }
    signal?.throwIfAborted();
    this.polled.set(meshName, sources);
    this.notifyPlacementChange(meshName);
  }

  /** MeshNode §5.2: selection and convergence use the same discovery observation. */
  async selectPlacement(
    meshName: string,
    kind: 'actor' | 'user_spot',
    stableType: string,
    node: ZLinkBackendMeshNode | undefined,
    signal?: AbortSignal,
    excludedNodeRids?: ReadonlySet<string>
  ): Promise<ZLinkMeshNodeDescriptor | undefined> {
    let waitedForEmpty = false;
    for (;;) {
      signal?.throwIfAborted();
      const candidates = [...(this.polled.get(meshName)?.values() ?? [])]
        .filter((source) => this.monotonicNowMs() < source.leaseDeadlineMs)
        .map((source) => source.descriptor)
        .filter((descriptor) => {
          const capability = descriptor.objectCapabilities.find(
            (value) => value.objectKind === kind && value.stableType === stableType
          );
          const capacity =
            kind === 'actor'
              ? descriptor.populationCapacity.actors
              : descriptor.populationCapacity.spots;
          const typeCapacity =
            kind === 'actor'
              ? undefined
              : descriptor.populationCapacity.spotTypes.find(
                  (value) => value.objectKind === kind && value.stableType === stableType
                );
          return (
            descriptor.objectRole === 'server' &&
            descriptor.placementWeight > 0 &&
            excludedNodeRids?.has(encodeRoutingIdStorageHex(descriptor.rid)) !== true &&
            capability !== undefined &&
            (kind !== 'actor' ||
              (descriptor.entrySpotId !== undefined && capability.entryAccepted)) &&
            (capacity.limit === 0 || capacity.active + capacity.reserved < capacity.limit) &&
            (typeCapacity === undefined ||
              typeCapacity.limit === 0 ||
              typeCapacity.active + typeCapacity.reserved < typeCapacity.limit)
          );
        });
      if (candidates.length === 0) {
        // The cache may predate the first poll of a peer (startup or after clear()); give
        // discovery one publication before reporting that no node can take the object.
        if (waitedForEmpty) return undefined;
        waitedForEmpty = true;
        await this.waitForPlacementChange(meshName, signal);
        continue;
      }
      const status = node?.status();
      const selected = selectLocationPlacementDescriptor(
        candidates.filter(
          (descriptor) =>
            descriptor.state === ZLinkFrameworkRuntimeState.Serving &&
            isPlacementTargetReady(node, status, descriptor)
        )
      );
      if (selected !== undefined) return selected;
      // Selection and subscription run synchronously in the discovery owner's event-loop turn.
      // A refresh cannot publish between the snapshot check and listener registration.
      await this.waitForPlacementChange(meshName, signal);
    }
  }

  clear(): void {
    this.polled.clear();
    for (const meshName of this.placementChanges.keys()) this.notifyPlacementChange(meshName);
  }

  private notifyPlacementChange(meshName: string): void {
    for (const notify of this.placementChanges.get(meshName) ?? []) notify();
  }

  private waitForPlacementChange(meshName: string, signal?: AbortSignal): Promise<void> {
    return new Promise((resolve, reject) => {
      let listeners = this.placementChanges.get(meshName);
      if (listeners === undefined) {
        listeners = new Set();
        this.placementChanges.set(meshName, listeners);
      }
      const finish = () => {
        listeners.delete(changed);
        if (listeners.size === 0) this.placementChanges.delete(meshName);
        signal?.removeEventListener('abort', aborted);
      };
      const changed = () => {
        finish();
        resolve();
      };
      const aborted = () => {
        finish();
        reject(signal?.reason);
      };
      listeners.add(changed);
      signal?.addEventListener('abort', aborted, { once: true });
      if (signal?.aborted === true) aborted();
    });
  }

  /**
   * Returns the recorded values, undefined when the sender has no live descriptor, or a promise
   * while the Store is read once for a sender the polled descriptors do not contain.
   */
  accept(
    meshName: string,
    nodeRid: RoutingId,
    nodeGeneration: bigint,
    nodeRidStorageHex: string,
    signal?: AbortSignal
  ): ZLinkRequestSourceFence | undefined | Promise<ZLinkRequestSourceFence | undefined> {
    throwIfAborted(signal);
    const known = this.polled.get(meshName)?.get(nodeRidStorageHex);
    if (known !== undefined && known.fence.nodeGeneration === nodeGeneration) {
      return this.monotonicNowMs() < known.leaseDeadlineMs ? known.fence : undefined;
    }
    return this.readOnce(meshName, nodeRid, nodeGeneration, nodeRidStorageHex, signal);
  }

  private async readOnce(
    meshName: string,
    nodeRid: RoutingId,
    nodeGeneration: bigint,
    nodeRidStorageHex: string,
    signal?: AbortSignal
  ): Promise<ZLinkRequestSourceFence | undefined> {
    // The Store boundary already classifies provider failures as Unavailable. Preserve their
    // cause for the ingress error reporter, and let programming errors propagate unchanged.
    const observed = this.polled.get(meshName);
    const descriptors = await awaitWithAbort(
      this.options.listLiveMeshNodes(meshName, signal),
      signal
    );
    const descriptor = descriptors.find(
      (value) => routingIdsEqual(value.rid, nodeRid) && value.lifecycleGeneration === nodeGeneration
    );
    const source =
      descriptor === undefined ? undefined : await this.polledSource(descriptor, signal);
    throwIfAborted(signal);
    if (source === undefined) return undefined;
    // A point read must not restore a descriptor removed by a later discovery publication.
    if (this.polled.get(meshName) !== observed) return source.fence;
    let sources = observed;
    if (sources === undefined) {
      sources = new Map();
      this.polled.set(meshName, sources);
    }
    sources.set(nodeRidStorageHex, source);
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
    const remainingMs = await awaitWithAbort(
      this.options.remainingOwnerTokenLeaseMs(
        { ownerId: descriptor.ownerId, leaseGeneration: descriptor.leaseGeneration },
        signal
      ),
      signal
    );
    if (remainingMs <= 0) return undefined;
    return {
      descriptor,
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

function selectLocationPlacementDescriptor(
  descriptors: readonly ZLinkMeshNodeDescriptor[]
): ZLinkMeshNodeDescriptor | undefined {
  const total = descriptors.reduce(
    (sum, descriptor) => sum + BigInt(descriptor.placementWeight),
    0n
  );
  if (total === 0n) return undefined;
  let point = randomBigIntBelow(total);
  for (const descriptor of descriptors) {
    const weight = BigInt(descriptor.placementWeight);
    if (point < weight) return descriptor;
    point -= weight;
  }
  return descriptors.at(-1);
}

function isPlacementTargetReady(
  node: ZLinkBackendMeshNode | undefined,
  localStatus: ReturnType<ZLinkBackendMeshNode['status']> | undefined,
  descriptor: ZLinkMeshNodeDescriptor
): boolean {
  if (node === undefined) return true;
  if (
    localStatus !== undefined &&
    routingIdsEqual(localStatus.routingId, descriptor.rid) &&
    localStatus.lifecycleGeneration === descriptor.lifecycleGeneration
  ) {
    return true;
  }
  return node.isPeerRouteReady?.(descriptor.rid, descriptor.lifecycleGeneration) ?? true;
}

function randomBigIntBelow(exclusiveUpperBound: bigint): bigint {
  if (exclusiveUpperBound <= 0n) {
    throw new RangeError('Random selection upper bound must be positive.');
  }
  const bitLength = exclusiveUpperBound.toString(2).length;
  const byteLength = Math.ceil(bitLength / 8);
  const highBitMask = 0xff >> (byteLength * 8 - bitLength);
  for (;;) {
    const bytes = randomBytes(byteLength);
    bytes[0] = bytes[0]! & highBitMask;
    const value = BigInt(`0x${bytes.toString('hex')}`);
    if (value < exclusiveUpperBound) return value;
  }
}
