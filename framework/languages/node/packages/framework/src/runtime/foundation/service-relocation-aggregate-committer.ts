import type {
  ZLinkAggregateFence,
  ZLinkAggregateId,
  ZLinkAggregateParticipant,
  ZLinkAuthorityKey,
  ZLinkAuthoritySnapshot,
  ZLinkCapacityVector,
  ZLinkLocationOwnerToken,
  ZLinkMeshNodeDescriptorKey
} from '../../contracts/Locations';
import type { ZLinkAuthorityStore, ZLinkObjectCreationStore } from '../locations/internal-store-contracts';
import {
  inventoryDigest,
  type ServiceRelocationEnvelope
} from './service-relocation-runtime';

export interface ServiceRelocationAggregateParticipantPlan {
  readonly key: ZLinkAuthorityKey;
  readonly expected: ZLinkAuthoritySnapshot;
  readonly ownerTransition: 'preserve' | 'newOwner';
  readonly authorityPayload: Uint8Array;
  readonly membershipMutation: Uint8Array;
}

export interface ServiceRelocationAggregatePlan {
  readonly envelope: ServiceRelocationEnvelope;
  readonly participants: readonly ServiceRelocationAggregateParticipantPlan[];
  readonly targetDescriptor: ZLinkMeshNodeDescriptorKey;
  readonly targetDescriptorLifecycleGeneration: bigint;
  readonly capacity: ZLinkCapacityVector;
  readonly targetOwner: ZLinkLocationOwnerToken;
}

export interface ServicePreparedRelocationAggregate {
  readonly fence: ZLinkAggregateFence;
  readonly plan: ServiceRelocationAggregatePlan;
}

type AggregateStore = Pick<
  ZLinkObjectCreationStore,
  'prepareAggregate' | 'commitAggregate' | 'abortAggregate'
> & Pick<ZLinkAuthorityStore, 'readAuthority'>;

/** Commits every owner and membership row through one Location Store fence. */
export class ServiceRelocationAggregateCommitter {
  constructor(private readonly store: AggregateStore) {}

  async prepare(
    plan: ServiceRelocationAggregatePlan,
    signal?: AbortSignal
  ): Promise<ServicePreparedRelocationAggregate> {
    const participants = validateAndEncodeParticipants(plan);
    const aggregateId = aggregateIdentity(plan.envelope.aggregateId);
    const result = await this.store.prepareAggregate({
      aggregateId,
      aggregateGeneration: plan.envelope.aggregateGeneration,
      participants,
      inventoryDigest: Buffer.from(
        inventoryDigest(plan.envelope.participants, plan.envelope.memberships),
        'hex'
      ),
      targetDescriptor: plan.targetDescriptor,
      targetDescriptorLifecycleGeneration: plan.targetDescriptorLifecycleGeneration,
      capacity: plan.capacity,
      targetOwner: plan.targetOwner
    }, signal);
    if (result.kind !== 'prepared' && result.kind !== 'alreadyPrepared') {
      throw new Error(`Location Store rejected relocation aggregate prepare: ${result.kind}.`);
    }
    if (
      result.fence.aggregateId.value !== aggregateId.value
      || result.fence.aggregateGeneration !== plan.envelope.aggregateGeneration
    ) {
      throw new Error('Location Store returned a different relocation aggregate fence.');
    }
    return { fence: result.fence, plan };
  }

  async commit(
    prepared: ServicePreparedRelocationAggregate,
    signal?: AbortSignal
  ): Promise<ReadonlyMap<string, ZLinkAuthoritySnapshot>> {
    try {
      await this.commitFence(prepared.fence, signal);
    } catch (firstError) {
      // A lost commit response is reconciled by repeating the exact durable fence.
      try {
        await this.commitFence(prepared.fence, signal);
      } catch {
        throw firstError;
      }
    }
    return await this.readCommittedParticipants(prepared.plan, signal);
  }

  async abort(
    prepared: ServicePreparedRelocationAggregate,
    signal?: AbortSignal
  ): Promise<void> {
    const result = await this.store.abortAggregate(prepared.fence, signal);
    if (result.kind !== 'aborted' && result.kind !== 'alreadyAborted') {
      throw new Error(`Location Store rejected relocation aggregate abort: ${result.kind}.`);
    }
  }

  private async commitFence(fence: ZLinkAggregateFence, signal?: AbortSignal): Promise<void> {
    const result = await this.store.commitAggregate(fence, signal);
    if (result.kind !== 'committed' && result.kind !== 'alreadyCommitted') {
      throw new Error(`Location Store rejected relocation aggregate commit: ${result.kind}.`);
    }
  }

  private async readCommittedParticipants(
    plan: ServiceRelocationAggregatePlan,
    signal?: AbortSignal
  ): Promise<ReadonlyMap<string, ZLinkAuthoritySnapshot>> {
    const committed = new Map<string, ZLinkAuthoritySnapshot>();
    for (const participant of plan.participants) {
      const current = await this.store.readAuthority(participant.key, signal);
      if (
        current.kind !== 'snapshot'
        || current.objectGeneration !== participant.expected.objectGeneration
        || current.storeVersion.value === participant.expected.storeVersion.value
        || !Buffer.from(current.payload).equals(Buffer.from(participant.authorityPayload))
      ) {
        throw new Error('Committed relocation aggregate does not match its exact participant plan.');
      }
      if (participant.ownerTransition === 'newOwner') {
        if (
          current.ownerId !== plan.targetOwner.ownerId
          || current.ownerLeaseGeneration !== plan.targetOwner.leaseGeneration
          || current.authorityOwnerGeneration <= participant.expected.authorityOwnerGeneration
        ) {
          throw new Error('Committed relocation participant has a different owner fence.');
        }
      } else if (
        current.ownerId !== participant.expected.ownerId
        || current.ownerLeaseGeneration !== participant.expected.ownerLeaseGeneration
        || current.authorityOwnerGeneration !== participant.expected.authorityOwnerGeneration
      ) {
        throw new Error('Preserved relocation participant changed its owner fence.');
      }
      committed.set(participant.key.value, current);
    }
    return committed;
  }
}

function validateAndEncodeParticipants(
  plan: ServiceRelocationAggregatePlan
): readonly ZLinkAggregateParticipant[] {
  const envelopeByKey = new Map(plan.envelope.participants.map(value => [value.key, value]));
  const ordered = [...plan.participants].sort((left, right) =>
    left.key.value.localeCompare(right.key.value));
  if (
    ordered.length !== plan.envelope.participants.length
    || new Set(ordered.map(({ key }) => key.value)).size !== ordered.length
  ) {
    throw new TypeError('Relocation aggregate plan must cover every participant exactly once.');
  }
  return ordered.map(participant => {
    const envelope = envelopeByKey.get(participant.key.value);
    if (
      envelope === undefined
      || envelope.objectGeneration !== participant.expected.objectGeneration
      || envelope.authorityOwnerGeneration !== participant.expected.authorityOwnerGeneration
    ) {
      throw new TypeError('Relocation aggregate participant fence differs from its envelope.');
    }
    return {
      authorityKey: participant.key,
      expectedStoreVersion: participant.expected.storeVersion,
      ownerTransition: participant.ownerTransition,
      authorityPayload: Buffer.from(participant.authorityPayload),
      membershipMutation: Buffer.from(participant.membershipMutation)
    };
  });
}

function aggregateIdentity(value: string): ZLinkAggregateId {
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(value)) {
    throw new TypeError('Relocation aggregate id must be a lowercase canonical UUID v4.');
  }
  return { value } as ZLinkAggregateId;
}
