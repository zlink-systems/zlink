import type {
  ZLinkAuthorityKey,
  ZLinkAuthoritySnapshot,
  ZLinkLocationOwnerToken,
  ZLinkRelocationCapacityFence
} from '../../contracts/Locations';
import {
  ServiceDurableRelocationRuntime,
  type ServiceRelocationEnvelope,
  type ServiceRelocationPublication,
  type ServiceRelocationSuccessorProgress
} from './service-relocation-runtime';

export interface ServiceRelocationStaging {
  readonly id: string;
}

export interface ServiceRelocationRestoreOwner<TStaging extends ServiceRelocationStaging> {
  prepare(
    envelope: ServiceRelocationEnvelope,
    signal?: AbortSignal,
    publication?: ServiceRelocationPublication
  ): Promise<TStaging>;
  publish(
    staging: TStaging,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<void>;
  replayAcceptedJournal(
    staging: TStaging,
    signal?: AbortSignal
  ): Promise<void>;
  normalize(
    staging: TStaging,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<void>;
  openAdmission(
    staging: TStaging,
    signal?: AbortSignal
  ): Promise<void>;
  abort(staging: TStaging): Promise<void> | void;
}

export interface ServiceRelocationSourceCompletion<TStaging extends ServiceRelocationStaging> {
  /**
   * Completes durable source cleanup and publishes the final Completed
   * authority while keeping the relocation reference available for recovery.
   */
  complete(
    staging: TStaging,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot>;
}

/** Writes durable source-cleanup completion before route replacement. */
export class ServiceRelocationSourceAuthorityWriter<
  TStaging extends ServiceRelocationStaging
> implements ServiceRelocationSourceCompletion<TStaging> {
  constructor(
    private readonly durable: ServiceDurableRelocationRuntime,
    private readonly authorityKey: (staging: TStaging) => ZLinkAuthorityKey,
    private readonly progress?: (
      staging: TStaging
    ) => ServiceRelocationSuccessorProgress | undefined
  ) {}

  complete(
    staging: TStaging,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    return this.durable.completeSourceCleanup(
      this.authorityKey(staging),
      authority,
      this.progress?.(staging),
      signal
    );
  }
}

export interface ServiceRelocationRouteReplacement<TStaging extends ServiceRelocationStaging> {
  replace(
    staging: TStaging,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<void>;
}

export interface ServiceRelocationTerminalRelay<
  TStaging extends ServiceRelocationStaging
> {
  /** Relays only terminal bytes already recorded in the completed root. */
  relayCaptured(
    staging: TStaging,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot | void>;
}

export interface ServiceRelocationRecoveryRetention<
  TStaging extends ServiceRelocationStaging
> {
  /**
   * Completes only after terminal/reply relay acknowledgement or the exact
   * source lease expiry makes the recovery pointer safe to release.
   */
  waitUntilReleasable(
    staging: TStaging,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<void>;
}

export interface ServiceRelocationAuthorityCommit {
  commit(
    key: ZLinkAuthorityKey,
    published: ZLinkAuthoritySnapshot,
    targetOwner: ZLinkLocationOwnerToken,
    envelope: ServiceRelocationEnvelope,
    relocationCapacityFence: ZLinkRelocationCapacityFence | undefined,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot>;
}

export interface ServiceCommittedRelocation<TStaging extends ServiceRelocationStaging> {
  readonly staging: TStaging;
  readonly authority: ZLinkAuthoritySnapshot;
}

export class ServiceRelocationPostCommitError extends Error {
  constructor(
    readonly authority: ZLinkAuthoritySnapshot,
    readonly staging: ServiceRelocationStaging,
    readonly cause: unknown
  ) {
    super('Relocation owner committed, but target publication or route replacement failed.');
    this.name = 'ServiceRelocationPostCommitError';
  }
}

/**
 * Materializes a relocation into a hidden target owner, commits authority once,
 * then publishes the owner and replaces routes. A failure after the authority
 * CAS is never converted into a source rollback.
 */
export class ServiceRelocationCoordinator<TStaging extends ServiceRelocationStaging> {
  constructor(
    private readonly durable: ServiceDurableRelocationRuntime,
    private readonly owner: ServiceRelocationRestoreOwner<TStaging>,
    private readonly source: ServiceRelocationSourceCompletion<TStaging>,
    private readonly terminalRelay: ServiceRelocationTerminalRelay<TStaging>,
    private readonly routes: ServiceRelocationRouteReplacement<TStaging>,
    private readonly recovery: ServiceRelocationRecoveryRetention<TStaging>,
    private readonly authorityCommit?: ServiceRelocationAuthorityCommit
  ) {}

  /**
   * Publishes the captured immutable root, then reserves and restores the hidden
   * target before the owner commit. This is the production entry for a freshly
   * captured relocation; `restoreAndCommit` is reserved for recovery of an
   * already published root.
   */
  async captureRestoreAndCommit(
    key: ZLinkAuthorityKey,
    expected: ZLinkAuthoritySnapshot,
    targetOwner: ZLinkLocationOwnerToken,
    envelope: ServiceRelocationEnvelope,
    relocationCapacityFence?: ZLinkRelocationCapacityFence | ((
      published: ZLinkAuthoritySnapshot,
      signal?: AbortSignal
    ) => Promise<ZLinkRelocationCapacityFence>),
    signal?: AbortSignal
  ): Promise<ServiceCommittedRelocation<TStaging>> {
    let staging: TStaging | undefined;
    let published: ZLinkAuthoritySnapshot | undefined;
    let committed: ZLinkAuthoritySnapshot | undefined;
    try {
      const captured = await this.durable.captureAndPublish(
        key,
        expected,
        targetOwner,
        envelope,
        signal
      );
      published = captured.authority;
      staging = await this.owner.prepare(envelope, signal, captured.publication);
      const preparedStaging = staging;
      const capacityFence = typeof relocationCapacityFence === 'function'
        ? await relocationCapacityFence(published, signal)
        : relocationCapacityFence;
      committed = await this.commitAuthority(
        key, published, targetOwner, envelope, capacityFence, signal);
      return await this.finishCommitted(key, { staging: preparedStaging, authority: committed }, signal);
    } catch (error) {
      if (committed !== undefined) {
        if (error instanceof ServiceRelocationPostCommitError) throw error;
        if (staging === undefined) throw error;
        throw new ServiceRelocationPostCommitError(committed, staging, error);
      }
      if (published !== undefined) {
        await this.durable.release(key, published).catch(() => undefined);
      }
      if (staging !== undefined) await this.owner.abort(staging);
      throw error;
    }
  }

  async restoreAndCommit(
    key: ZLinkAuthorityKey,
    published: ZLinkAuthoritySnapshot,
    targetOwner: ZLinkLocationOwnerToken,
    relocationCapacityFence?: ZLinkRelocationCapacityFence,
    signal?: AbortSignal
  ): Promise<ServiceCommittedRelocation<TStaging>> {
    const envelope = await this.durable.restore(published, signal);
    const staging = await this.owner.prepare(
      envelope,
      signal,
      this.durable.readPublication(published)
    );
    let committed: ZLinkAuthoritySnapshot | undefined;
    try {
      committed = await this.commitAuthority(
        key, published, targetOwner, envelope, relocationCapacityFence, signal);
      return await this.finishCommitted(key, { staging, authority: committed }, signal);
    } catch (error) {
      if (committed !== undefined) {
        if (error instanceof ServiceRelocationPostCommitError) throw error;
        throw new ServiceRelocationPostCommitError(committed, staging, error);
      }
      await this.owner.abort(staging);
      throw error;
    }
  }

  /**
   * Continues an already committed target after a process or transport fault.
   * Every callback in this phase must accept an exact duplicate for the same
   * staging id and authority fence.
   */
  async resumeCommitted(
    key: ZLinkAuthorityKey,
    committed: ServiceCommittedRelocation<TStaging>,
    signal?: AbortSignal
  ): Promise<ServiceCommittedRelocation<TStaging>> {
    return await this.finishCommitted(key, committed, signal);
  }

  private async finishCommitted(
    key: ZLinkAuthorityKey,
    committed: ServiceCommittedRelocation<TStaging>,
    signal?: AbortSignal
  ): Promise<ServiceCommittedRelocation<TStaging>> {
    const { staging } = committed;
    let authority = committed.authority;
    try {
      await this.owner.replayAcceptedJournal(staging, signal);
      // Target objects remain hidden until state, queued work, timers, and
      // Session barriers have been restored. Registry visibility is the last
      // step before source completion and admission opening.
      await this.owner.publish(staging, authority, signal);
      authority = await this.source.complete(staging, authority, signal);
      authority = await this.terminalRelay.relayCaptured(staging, authority, signal) ?? authority;
      await this.routes.replace(staging, authority, signal);
      await this.owner.normalize(staging, authority, signal);
      await this.owner.openAdmission(staging, signal);
      await this.recovery.waitUntilReleasable(staging, authority, signal);
      authority = await this.durable.release(key, authority, signal);
      return { staging, authority };
    } catch (error) {
      throw new ServiceRelocationPostCommitError(authority, staging, error);
    }
  }

  private commitAuthority(
    key: ZLinkAuthorityKey,
    published: ZLinkAuthoritySnapshot,
    targetOwner: ZLinkLocationOwnerToken,
    envelope: ServiceRelocationEnvelope,
    relocationCapacityFence: ZLinkRelocationCapacityFence | undefined,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    return this.authorityCommit?.commit(
      key,
      published,
      targetOwner,
      envelope,
      relocationCapacityFence,
      signal
    ) ?? this.durable.commitOwner(
      key,
      published,
      targetOwner,
      relocationCapacityFence,
      signal
    );
  }

}
