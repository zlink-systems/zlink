import type { ZLinkLocationOwnerToken } from './internal-location-contracts';
import { ZLinkFrameworkErrorKind, ZLinkFrameworkException } from '../../contracts';

export class ZLinkOwnerLeaseReleaseDelayPendingError extends ZLinkFrameworkException {
  constructor() {
    super(
      ZLinkFrameworkErrorKind.Unavailable,
      'Fixed RID claim is waiting for the previous owner lease release delay.'
    );
  }
}

interface ReleaseDelayObservation {
  readonly owner: ZLinkLocationOwnerToken | undefined;
  readonly t0: number;
}

/** One local monotonic observation window for an invalid owner or missing descriptor. */
export class ZLinkOwnerLeaseReleaseDelay {
  private readonly observations = new Map<string, ReleaseDelayObservation>();

  constructor(
    private readonly delayMs: number,
    private readonly monotonicNowMs: () => number = () => performance.now()
  ) {}

  nowMs(): number {
    return this.monotonicNowMs();
  }

  get observationCountForTesting(): number {
    return this.observations.size;
  }

  allows(
    scope: string,
    owner: ZLinkLocationOwnerToken | undefined,
    ownerLeaseLive = false,
    observedAtMs = this.monotonicNowMs()
  ): boolean {
    if (owner !== undefined && ownerLeaseLive) {
      this.observations.delete(scope);
      return true;
    }

    let observation = this.observations.get(scope);
    if (owner === undefined) {
      if (observation?.owner !== undefined) {
        observation = { owner: undefined, t0: observedAtMs };
        this.observations.set(scope, observation);
      }
    } else if (
      observation === undefined ||
      observation.owner?.ownerId !== owner.ownerId ||
      observation.owner.leaseGeneration !== owner.leaseGeneration
    ) {
      observation = {
        owner: { ownerId: owner.ownerId, leaseGeneration: owner.leaseGeneration },
        t0: observedAtMs
      };
      this.observations.set(scope, observation);
    }
    if (observation === undefined) {
      observation = { owner: undefined, t0: observedAtMs };
      this.observations.set(scope, observation);
    }
    return this.delayElapsed(observation, observedAtMs);
  }

  clear(scope: string): void {
    this.observations.delete(scope);
  }

  observation(scope: string): ReleaseDelayObservation | undefined {
    return this.observations.get(scope);
  }

  clearIfUnchanged(scope: string, observation: ReleaseDelayObservation | undefined): void {
    if (this.observations.get(scope) === observation) this.observations.delete(scope);
  }

  clearAuthority(authorityKey: string): void {
    this.clear(ownerLeaseReleaseDelayScope.authority(authorityKey));
  }

  clearAuthorityIfOwner(authorityKey: string, owner: ZLinkLocationOwnerToken): void {
    this.clearIfOwner(ownerLeaseReleaseDelayScope.authority(authorityKey), owner);
  }

  clearUserSpotMemberIfOwner(
    spotId: string,
    objectGeneration: bigint,
    authorityKey: string,
    owner: ZLinkLocationOwnerToken
  ): void {
    this.clearIfOwner(
      ownerLeaseReleaseDelayScope.userSpotMember(spotId, objectGeneration, authorityKey),
      owner
    );
  }

  clearExcept(scopePrefix: string, retainedScopes: ReadonlySet<string>): void {
    for (const scope of this.observations.keys()) {
      if (scope.startsWith(scopePrefix) && !retainedScopes.has(scope)) {
        this.observations.delete(scope);
      }
    }
  }

  private delayElapsed(observation: ReleaseDelayObservation, observedAtMs: number): boolean {
    return observedAtMs >= observation.t0 + this.delayMs;
  }

  private clearIfOwner(scope: string, owner: ZLinkLocationOwnerToken): void {
    const observation = this.observations.get(scope);
    if (
      observation?.owner?.ownerId === owner.ownerId &&
      observation.owner.leaseGeneration === owner.leaseGeneration
    ) {
      this.observations.delete(scope);
    }
  }
}

export const ownerLeaseReleaseDelayScope = {
  authority(key: string): string {
    return `authority\0${key}`;
  },
  meshNode(meshName: string, rid: string): string {
    return `mesh-node\0${meshName}\0${rid}`;
  },
  userSpotMembers(spotId: string, objectGeneration: bigint): string {
    return `user-spot-member\0${spotId}\0${objectGeneration}\0`;
  },
  userSpotMember(spotId: string, objectGeneration: bigint, authorityKey: string): string {
    return `${this.userSpotMembers(spotId, objectGeneration)}${authorityKey}`;
  }
};
