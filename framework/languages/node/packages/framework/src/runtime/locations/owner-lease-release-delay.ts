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
  private readonly authorityObservations = new Map<string, number>();
  private readonly observations = new Map<string, ReleaseDelayObservation>();

  constructor(
    private readonly delayMs: number,
    private readonly monotonicNowMs: () => number = () => performance.now()
  ) {}

  nowMs(): number {
    return this.monotonicNowMs();
  }

  get observationCountForTesting(): number {
    return this.observations.size + this.authorityObservations.size;
  }

  allowsAuthority(owner: ZLinkLocationOwnerToken, observedAtMs = this.monotonicNowMs()): boolean {
    const key = JSON.stringify([owner.ownerId, owner.leaseGeneration.toString()]);
    const firstObserved = this.authorityObservations.get(key);
    if (firstObserved !== undefined) return this.delayElapsed(firstObserved, observedAtMs);
    this.authorityObservations.set(key, observedAtMs);
    return this.delayElapsed(observedAtMs, observedAtMs);
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
    return this.delayElapsed(observation.t0, observedAtMs);
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

  private delayElapsed(firstObserved: number, observedAtMs: number): boolean {
    return observedAtMs >= firstObserved + this.delayMs;
  }
}

export const ownerLeaseReleaseDelayScope = {
  meshNode(meshName: string, rid: string): string {
    return `mesh-node\0${meshName}\0${rid}`;
  }
};
