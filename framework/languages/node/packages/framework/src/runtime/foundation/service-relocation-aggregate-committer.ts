import type {
  AuthorityRelocation,
  ZLinkAggregateFence,
  ZLinkAggregateId,
  ZLinkAggregateParticipant,
  ZLinkAuthorityKey,
  ZLinkAuthoritySnapshot,
  ZLinkCapacityVector,
  ZLinkLocationOwnerToken,
  ZLinkMeshNodeDescriptorKey
} from '../locations/internal-location-contracts';
import type { ZLinkObjectCreationStore } from '../locations/internal-store-contracts';
import { inventoryDigest, type ServiceRelocationEnvelope } from './service-relocation-runtime';

export interface ServiceRelocationAggregateParticipantPlan {
  readonly key: ZLinkAuthorityKey;
  readonly expected: ZLinkAuthoritySnapshot;
  readonly ownerTransition: 'preserve' | 'newOwner';
  readonly authorityPayload: Uint8Array;
  readonly relocation?: AuthorityRelocation;
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

type AggregateStore = Pick<ZLinkObjectCreationStore, 'prepareAggregate' | 'abortAggregate'>;

/** Prepares and aborts relocation through one Location Store fence. */

export class ServiceRelocationAggregateCommitter {
  constructor(private readonly store: AggregateStore) {}

  async prepare(
    plan: ServiceRelocationAggregatePlan,
    signal?: AbortSignal,
    deadlineUnixMs?: bigint
  ): Promise<ServicePreparedRelocationAggregate> {
    const participants = validateAndEncodeParticipants(plan);
    const aggregateId = aggregateIdentity(plan.envelope.aggregateId);
    const request = {
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
    };
    const result = await this.store.prepareAggregate(request, signal, deadlineUnixMs);
    if (result.kind !== 'prepared' && result.kind !== 'alreadyPrepared') {
      throw new Error(`Location Store rejected relocation aggregate prepare: ${result.kind}.`);
    }
    if (
      result.fence.aggregateId.value !== aggregateId.value ||
      result.fence.aggregateGeneration !== plan.envelope.aggregateGeneration
    ) {
      throw new Error('Location Store returned a different relocation aggregate fence.');
    }
    return { fence: result.fence, plan };
  }

  async abort(prepared: ServicePreparedRelocationAggregate, signal?: AbortSignal): Promise<void> {
    const result = await this.store.abortAggregate(prepared.fence, signal);
    if (result.kind !== 'aborted' && result.kind !== 'alreadyAborted') {
      throw new Error(`Location Store rejected relocation aggregate abort: ${result.kind}.`);
    }
  }
}

function validateAndEncodeParticipants(
  plan: ServiceRelocationAggregatePlan
): readonly ZLinkAggregateParticipant[] {
  const envelopeByKey = new Map(plan.envelope.participants.map((value) => [value.key, value]));
  const ordered = [...plan.participants].sort((left, right) =>
    left.key.value.localeCompare(right.key.value)
  );
  if (
    ordered.length !== plan.envelope.participants.length ||
    new Set(ordered.map(({ key }) => key.value)).size !== ordered.length
  ) {
    throw new TypeError('Relocation aggregate plan must cover every participant exactly once.');
  }
  return ordered.map((participant) => {
    const envelope = envelopeByKey.get(participant.key.value);
    if (
      envelope === undefined ||
      envelope.objectGeneration !== participant.expected.objectGeneration ||
      envelope.authorityOwnerGeneration !== participant.expected.authorityOwnerGeneration
    ) {
      throw new TypeError('Relocation aggregate participant fence differs from its envelope.');
    }
    return {
      authorityKey: participant.key,
      expectedStoreVersion: participant.expected.storeVersion,
      ownerTransition: participant.ownerTransition,
      authorityPayload: Buffer.from(participant.authorityPayload),
      relocation: participant.relocation,
      membershipMutation: Buffer.from(participant.membershipMutation)
    };
  });
}

function aggregateIdentity(value: string): ZLinkAggregateId {
  if (
    !/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(value) ||
    /^0{8}-0{4}-0{4}-0{4}-0{12}$/.test(value)
  ) {
    throw new TypeError('Relocation aggregate id must be a lowercase non-zero 128-bit identity.');
  }
  return { value } as ZLinkAggregateId;
}
