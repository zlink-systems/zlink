import type { ZLinkLocationOwnerToken } from './internal-location-contracts';

interface InvalidOwnerObservation {
  readonly ownerId: string;
  readonly leaseGeneration: bigint;
  readonly t0: number;
}

/** One local monotonic observation window for invalid owner tokens. */
export class ZLinkOwnerLeaseReleaseDelay {
  private readonly observations = new Map<string, InvalidOwnerObservation>();

  constructor(
    private readonly delayMs: number,
    private readonly monotonicNowMs: () => number = () => performance.now()
  ) {}

  nowMs(): number {
    return this.monotonicNowMs();
  }

  allows(
    scope: string,
    owner: ZLinkLocationOwnerToken,
    ownerLeaseLive: boolean,
    observedAtMs = this.monotonicNowMs()
  ): boolean {
    if (ownerLeaseLive) {
      this.observations.delete(scope);
      return true;
    }

    let observation = this.observations.get(scope);
    if (
      observation === undefined ||
      observation.ownerId !== owner.ownerId ||
      observation.leaseGeneration !== owner.leaseGeneration
    ) {
      observation = {
        ownerId: owner.ownerId,
        leaseGeneration: owner.leaseGeneration,
        t0: observedAtMs
      };
      this.observations.set(scope, observation);
    }
    return observedAtMs >= observation.t0 + this.delayMs;
  }

  clear(scope: string): void {
    this.observations.delete(scope);
  }

  clearExcept(scopePrefix: string, retainedScopes: ReadonlySet<string>): void {
    for (const scope of this.observations.keys()) {
      if (scope.startsWith(scopePrefix) && !retainedScopes.has(scope)) {
        this.observations.delete(scope);
      }
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
  userSpotMembers(spotId: string): string {
    return `user-spot-member\0${spotId}\0`;
  },
  userSpotMember(spotId: string, objectGeneration: bigint, authorityKey: string): string {
    return `${this.userSpotMembers(spotId)}${objectGeneration}\0${authorityKey}`;
  }
};
