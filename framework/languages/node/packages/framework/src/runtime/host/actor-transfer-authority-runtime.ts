import {
  ActorTransferPhase,
  ActorTransferRole,
  type ActorTransferControlPayload
} from '../foundation/service-runtime-contracts';
import type { ServiceActorRef } from '../foundation/service-stateful-registry';
import type {
  ActorRef,
  RoutingId
} from '../../contracts';
import type { ZLinkActorLocation } from '../../contracts/Locations/Rows';
import type { ZLinkActorLocationStore } from '../locations/internal-store-contracts';
import type {
  ZLinkActorTransferRecord,
  ZLinkActorTransferStore
} from '../../contracts/Locations/ActorTransfer';
import { toFrameworkActorRef } from '../actors';

export type ZLinkActorTransferAuthorityStore =
  ZLinkActorTransferStore & ZLinkActorLocationStore;

export interface ZLinkActorTransferAuthorityRuntimeOptions {
  readonly store: () => ZLinkActorTransferAuthorityStore | undefined;
  readonly recoveryOwnerId: () => string | undefined;
  readonly recoveryLeaseTtlMs: number;
  readonly now?: () => Date;
}

/**
 * Projects Core transfer-control records into the durable location authority.
 * Core owns sealed tokens; this runtime persists only public transfer identity
 * and fences, then resumes terminal transitions after a recovery lease takeover.
 */
export class ZLinkActorTransferAuthorityRuntime {
  private readonly now: () => Date;

  constructor(private readonly options: ZLinkActorTransferAuthorityRuntimeOptions) {
    this.now = options.now ?? (() => new Date());
  }

  async handle(
    meshName: string,
    control: ActorTransferControlPayload,
    signal?: AbortSignal
  ): Promise<void> {
    if (control.actor === null || control.failureErrno !== 0 || control.resultCode !== 0) {
      return;
    }
    const store = this.requireStore();
    const ownerId = this.requireOwnerId();
    const actorId = control.actor.actorId;
    const transferId = transferIdString(control.transferId.high, control.transferId.low);

    if (
      control.role === ActorTransferRole.Target
      && (control.phase === ActorTransferPhase.Preparing || control.phase === ActorTransferPhase.Fenced)
    ) {
      await this.prepare(
        store,
        ownerId,
        meshName,
        actorId,
        transferId,
        control.actor,
        control.membershipEpoch,
        signal
      );
      return;
    }

    if (control.phase === ActorTransferPhase.Committed) {
      await this.transition(store, ownerId, meshName, actorId, transferId, 'committed', signal);
      return;
    }
    if (control.phase === ActorTransferPhase.Activated) {
      await this.transition(store, ownerId, meshName, actorId, transferId, 'activated', signal);
      return;
    }
    if (control.phase === ActorTransferPhase.Aborted) {
      await this.transition(store, ownerId, meshName, actorId, transferId, 'aborted', signal);
    }
  }

  private async prepare(
    store: ZLinkActorTransferAuthorityStore,
    ownerId: string,
    meshName: string,
    actorId: string,
    transferId: string,
    nativeTarget: ServiceActorRef,
    membershipEpoch: bigint,
    signal?: AbortSignal
  ): Promise<void> {
    const location = await store.resolveActor({ meshName, actorId }, signal);
    if (location === undefined) {
      throw new Error(`Actor transfer '${transferId}' has no source location row.`);
    }
    const target = toFrameworkActorRef(nativeTarget as never, meshName);
    validateTarget(location, target, membershipEpoch, transferId);
    const result = await store.prepareActorTransfer({
      meshName,
      actorId,
      transferId,
      source: location.actorRef,
      target,
      expectedActorGeneration: location.actorRef.objectGeneration,
      expectedMembershipEpoch: location.membershipEpoch,
      participants: new Set<RoutingId>([
        location.actorRef.nodeRid,
        target.nodeRid
      ]),
      recoveryOwnerId: ownerId,
      recoveryLeaseTtlMs: this.options.recoveryLeaseTtlMs
    }, signal);
    if (result.status !== 'stored') {
      throw new Error(`Actor transfer '${transferId}' prepare was rejected with status '${result.status}'.`);
    }
  }

  private async transition(
    store: ZLinkActorTransferAuthorityStore,
    ownerId: string,
    meshName: string,
    actorId: string,
    transferId: string,
    targetState: 'committed' | 'activated' | 'aborted',
    signal?: AbortSignal
  ): Promise<void> {
    let record = await store.resolveActorTransfer(meshName, actorId, signal);
    if (record === undefined || record.transferId !== transferId) {
      throw new Error(`Actor transfer '${transferId}' has no durable prepared record.`);
    }
    if (isAtOrBeyond(record, targetState)) {
      return;
    }
    if (record.recoveryOwnerId !== ownerId) {
      if (record.recoveryLeaseExpiresAt.getTime() > this.now().getTime()) {
        return;
      }
      const takeover = await store.takeOverActorTransfer(
        meshName,
        actorId,
        transferId,
        ownerId,
        this.options.recoveryLeaseTtlMs,
        signal
      );
      if (takeover.status !== 'stored' || takeover.record === undefined) {
        throw new Error(`Actor transfer '${transferId}' recovery takeover failed with status '${takeover.status}'.`);
      }
      record = takeover.record;
    }

    if (targetState === 'activated' && record.state === 'prepared') {
      record = requireStored(await store.commitActorTransfer(
        meshName, actorId, transferId, ownerId, signal
      ), transferId, 'commit');
    }
    const result = targetState === 'committed'
      ? await store.commitActorTransfer(meshName, actorId, transferId, ownerId, signal)
      : targetState === 'activated'
        ? await store.activateActorTransfer(meshName, actorId, transferId, ownerId, signal)
        : await store.abortActorTransfer(meshName, actorId, transferId, ownerId, signal);
    requireStored(result, transferId, targetState);
  }

  private requireStore(): ZLinkActorTransferAuthorityStore {
    const store = this.options.store();
    if (store === undefined) {
      throw new Error('Actor transfer control requires durable Actor transfer authority.');
    }
    return store;
  }

  private requireOwnerId(): string {
    const ownerId = this.options.recoveryOwnerId();
    if (ownerId === undefined || ownerId.length === 0) {
      throw new Error('Actor transfer control requires an active location owner.');
    }
    return ownerId;
  }
}

function validateTarget(
  location: ZLinkActorLocation,
  target: ActorRef,
  membershipEpoch: bigint,
  transferId: string
): void {
  if (
    target.actorId !== location.actorId
    || target.objectGeneration !== location.actorRef.objectGeneration
    || membershipEpoch !== location.membershipEpoch
  ) {
    throw new Error(`Actor transfer '${transferId}' does not match the source Actor generation and membership epoch.`);
  }
}

function requireStored(
  result: Awaited<ReturnType<ZLinkActorTransferStore['commitActorTransfer']>>,
  transferId: string,
  operation: string
): ZLinkActorTransferRecord {
  if (result.status !== 'stored' || result.record === undefined) {
    throw new Error(`Actor transfer '${transferId}' ${operation} failed with status '${result.status}'.`);
  }
  return result.record;
}

function isAtOrBeyond(
  record: ZLinkActorTransferRecord,
  target: 'committed' | 'activated' | 'aborted'
): boolean {
  if (record.state === target) return true;
  return target === 'committed' && (record.state === 'committed' || record.state === 'activated');
}

export function transferIdString(high: bigint, low: bigint): string {
  const hex = `${unsignedHex64(high)}${unsignedHex64(low)}`;
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function unsignedHex64(value: bigint): string {
  return BigInt.asUintN(64, value).toString(16).padStart(16, '0');
}
