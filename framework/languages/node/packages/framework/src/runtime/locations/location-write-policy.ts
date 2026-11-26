import { ZLinkLocationWriteIntent } from '../../contracts/Locations';
import {
  ZLinkInternalMeshNodeWriteIntent,
  type ZLinkMeshNodeWriteIntent
} from './domain-store-contract';

export function canTakeOverStoredLocation(
  intent: ZLinkMeshNodeWriteIntent,
  currentOwnerId: string,
  currentLeaseGeneration: bigint,
  nextOwnerId: string,
  nextLeaseGeneration: bigint,
  liveLeaseGeneration: bigint | undefined
): boolean {
  if (intent === ZLinkLocationWriteIntent.Takeover) {
    // An explicit handoff is fenced by the Store write and the next owner's lease.
    return true;
  }
  const recovery = intent === ZLinkInternalMeshNodeWriteIntent.RecoveryClaim;
  if (
    intent !== ZLinkLocationWriteIntent.NewClaim &&
    intent !== ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim &&
    !recovery
  ) {
    return false;
  }
  // Recovery replaces only this owner's row from an earlier lease generation.
  if (liveLeaseGeneration === undefined) return !recovery;
  return (
    currentOwnerId === nextOwnerId &&
    currentLeaseGeneration !== nextLeaseGeneration &&
    liveLeaseGeneration === nextLeaseGeneration
  );
}
