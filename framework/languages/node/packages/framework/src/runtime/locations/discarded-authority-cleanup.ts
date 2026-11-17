import { ZLinkSpotKind } from '../../contracts';
import { decodeActorAuthorityPayload } from '../actors/actor-authority-payload-codec';
import { decodeServiceReadySpotAuthority } from '../foundation/service-authority-payload-codec';
import { serviceRelocationAuthorityApplicationPayload } from '../foundation/service-relocation-runtime';
import { sameAuthorityIncarnation, type ZLinkDiscardedAuthorityFence } from './lifecycle-runtime';
import type { ZLinkAuthorityStore, ZLinkOwnerLeaseStore } from './internal-store-contracts';
import type {
  ZLinkAuthorityKey,
  ZLinkAuthoritySnapshot,
  ZLinkLocationOwnerToken,
  ZLinkAuthorityScanCursor
} from './internal-location-contracts';
import { ZLINK_PROVIDER_MAX_PAGE_SIZE } from '../../contracts/Locations/Stores';

/** Repository policy shared by the provider repository and the in-memory reference store. */
export async function cleanupDiscardedAuthority(
  store: ZLinkAuthorityStore & Pick<ZLinkOwnerLeaseStore, 'readOwnerLease'>,
  release: (current: ZLinkAuthoritySnapshot) => Promise<boolean>,
  key: ZLinkAuthorityKey,
  fence: ZLinkDiscardedAuthorityFence,
  owner: ZLinkLocationOwnerToken,
  signal?: AbortSignal
): Promise<boolean> {
  const current = await store.readAuthority(key, signal);
  if (current.kind !== 'snapshot' || !sameAuthorityIncarnation(current, fence)) return true;
  if (owner.ownerId !== fence.ownerId) return false;
  if (current.allocation.objectKind === 'user_spot') {
    const spot = decodeServiceReadySpotAuthority(current.payload);
    if (spot === undefined) return false;
    membershipSnapshot: for (;;) {
      let cursor: ZLinkAuthorityScanCursor | undefined;
      do {
        const page = await store.listAuthorities('', cursor, ZLINK_PROVIDER_MAX_PAGE_SIZE, signal);
        if (page.kind === 'scanExpired') continue membershipSnapshot;
        for (const entry of page.items) {
          if (entry.snapshot.allocation.objectKind !== 'actor') continue;
          const actor = decodeActorAuthorityPayload(
            serviceRelocationAuthorityApplicationPayload(entry.snapshot.payload)
          );
          if (
            actor?.currentSpotKind === ZLinkSpotKind.User &&
            actor.currentSpotId === spot.spotId &&
            actor.currentSpotGeneration === current.objectGeneration
          )
            return false;
        }
        cursor = page.nextCursor;
      } while (cursor !== undefined);
      break;
    }
  }
  const lease = await store.readOwnerLease(current.ownerId, signal);
  const live =
    lease.kind === 'found' &&
    lease.token.ownerId === current.ownerId &&
    lease.token.leaseGeneration === current.ownerLeaseGeneration &&
    lease.leaseExpiresAt > lease.storeNow;
  if (live) {
    if (current.ownerLeaseGeneration !== owner.leaseGeneration) return false;
    return (
      (await store.compareExchangeAuthority(key, current.storeVersion, { kind: 'delete' }, signal))
        .kind === 'deleted'
    );
  }
  return await release(current);
}
