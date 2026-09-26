import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from './framework-errors-internal';

/** Current activation admission count and limit of one MeshNode. */
export interface ZLinkActivationConcurrency {
  readonly active: number;
  readonly limit: number;
}

interface ZLinkActivationRecord {
  readonly meshName: string;
  readonly limit: number;
  active: number;
}

/**
 * MeshNode §5.1 "Pending activation": the single per-MeshNode record of activation admissions.
 *
 * Actor creation, User Spot creation, Instance Spot cold activation and a relocation target's
 * Restore each hold one admission from the moment the target MeshNode receives the operation
 * until Ready or its target commit, or until it ends by rejection, failure or cleanup. Entry Spot
 * and Actor Join never acquire one. The limit is enforced here, and the status current value and
 * the placement `IsAvailable` headroom both read this record.
 */
export class ZLinkActivationAdmission {
  private readonly records = new Map<string, ZLinkActivationRecord>();

  constructor(
    private readonly limitOf: (meshName: string) => number,
    private readonly changed?: (meshName: string) => void
  ) {}

  current(meshName: string): ZLinkActivationConcurrency {
    const record = this.records.get(meshName);
    return { active: record?.active ?? 0, limit: record?.limit ?? this.limitOf(meshName) };
  }

  hasHeadroom(meshName: string): boolean {
    const { active, limit } = this.current(meshName);
    return active < limit;
  }

  /**
   * Holds one admission for `meshName`. A MeshNode without headroom does not accept a new
   * admission (MeshNode §5.1): the operation fails as Unavailable at once. The returned release
   * is idempotent.
   */
  async acquire(meshName: string, signal?: AbortSignal): Promise<() => void> {
    signal?.throwIfAborted();
    const record = this.record(meshName);
    if (record.active >= record.limit) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted,
        `MeshNode '${meshName}' has no activation admission headroom.`
      );
    }
    record.active += 1;
    this.changed?.(meshName);
    let released = false;
    return () => {
      if (released) return;
      released = true;
      record.active -= 1;
      this.changed?.(meshName);
    };
  }

  private record(meshName: string): ZLinkActivationRecord {
    let record = this.records.get(meshName);
    if (record === undefined) {
      record = { meshName, limit: this.limitOf(meshName), active: 0 };
      this.records.set(meshName, record);
    }
    return record;
  }
}
