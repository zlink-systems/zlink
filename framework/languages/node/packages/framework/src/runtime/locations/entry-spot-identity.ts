import type { ZLinkMeshNodeDescriptor } from '../../contracts';
import {
  ZLinkInternalMeshNodeWriteIntent,
  type ZLinkMeshNodeWriteIntent
} from './domain-store-contract';
import {
  ZLinkLocationWriteIntent,
  type ZLinkLocationOwnerToken
} from './internal-location-contracts';

export function isEntrySpotIdentityNewClaim(intent: ZLinkMeshNodeWriteIntent): boolean {
  return (
    intent === ZLinkLocationWriteIntent.NewClaim ||
    intent === ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim
  );
}

export interface EntrySpotIdentityClaim {
  readonly descriptorKey: string;
  readonly descriptorLifecycleGeneration: string;
  readonly ownerId: string;
  readonly ownerLeaseGeneration: string;
}

export function entrySpotIdentityClaim(
  descriptorKey: string,
  descriptor: ZLinkMeshNodeDescriptor
): EntrySpotIdentityClaim {
  return {
    descriptorKey,
    descriptorLifecycleGeneration: descriptor.lifecycleGeneration.toString(),
    ownerId: descriptor.ownerId,
    ownerLeaseGeneration: descriptor.leaseGeneration.toString()
  };
}

export function matchesEntrySpotIdentityClaim(
  left: EntrySpotIdentityClaim,
  right: EntrySpotIdentityClaim
): boolean {
  return (
    left.descriptorKey === right.descriptorKey &&
    left.descriptorLifecycleGeneration === right.descriptorLifecycleGeneration &&
    left.ownerId === right.ownerId &&
    left.ownerLeaseGeneration === right.ownerLeaseGeneration
  );
}

export function entrySpotIdentityOwner(claim: EntrySpotIdentityClaim): ZLinkLocationOwnerToken {
  return { ownerId: claim.ownerId, leaseGeneration: BigInt(claim.ownerLeaseGeneration) };
}
