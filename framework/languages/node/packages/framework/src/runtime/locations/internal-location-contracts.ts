export * from '../../contracts/Locations';
import type { ZLinkObjectReserveRequest } from '../../contracts/Locations/Authority';

export type ZLinkEndedOwnerReleaseIntent = Pick<ZLinkObjectReserveRequest, 'key'> & {
  readonly intent: Pick<ZLinkObjectReserveRequest['intent'], 'stableType'>;
  readonly target: Pick<
    ZLinkObjectReserveRequest['target'],
    'meshName' | 'nodeRid' | 'nodeLifecycleGeneration'
  >;
};
export type * from '../../contracts/Locations/Authority';
export type {
  ZLinkActorLocation,
  ZLinkPeerLocation,
  ZLinkRouteLocation,
  ZLinkSpotLocation
} from '../../contracts/Locations/Rows';
export type {
  ZLinkActorLocationFilter,
  ZLinkActorLocationKey,
  ZLinkLocationKey,
  ZLinkPeerLocationFilter,
  ZLinkPeerLocationKey,
  ZLinkRouteLocationFilter,
  ZLinkRouteLocationKey,
  ZLinkSpotLocationFilter,
  ZLinkSpotLocationKey
} from '../../contracts/Locations/Keys';
export { ZLinkLocationAutoConnectType, ZLinkRouteKind } from '../../contracts/Locations/Values';
export type { ZLinkPeerLocationResolver } from '../../contracts/Locations/Resolvers';
export type { ZLinkLocationChangeStampScope } from '../../contracts/Locations/Watch';

import type * as AuthorityContract from '../../contracts/Locations/Authority';
import type { AuthorityRelocation } from './authority-relocation-record';
export type { AuthorityRelocation } from './authority-relocation-record';

export type ZLinkAuthoritySnapshot = AuthorityContract.ZLinkAuthoritySnapshot & {
  readonly relocation?: AuthorityRelocation;
};
export type ZLinkAuthorityReadResult =
  { readonly kind: 'missing'; readonly storeNow: Date } | ZLinkAuthoritySnapshot;
export type ZLinkAuthorityMutation = AuthorityContract.ZLinkAuthorityMutation & {
  readonly relocation?: AuthorityRelocation | null;
};
export type ZLinkAuthorityCompareExchangeResult =
  | ({ readonly kind: 'stored' } & Omit<ZLinkAuthoritySnapshot, 'kind'>)
  | Exclude<
      AuthorityContract.ZLinkAuthorityCompareExchangeResult,
      { readonly kind: 'stored' } | { readonly kind: 'conflict' }
    >
  | {
      readonly kind: 'conflict';
      readonly current: ZLinkAuthorityReadResult;
      readonly membershipFailure?:
        | import('../../contracts/Errors').ZLinkFrameworkErrorKind.NotFound
        | import('../../contracts/Errors').ZLinkFrameworkErrorKind.Unavailable;
    };
export type ZLinkAggregateParticipant = AuthorityContract.ZLinkAggregateParticipant & {
  readonly relocation?: AuthorityRelocation;
};
export type ZLinkAggregatePrepareRequest = Omit<
  AuthorityContract.ZLinkAggregatePrepareRequest,
  'participants' | 'inventoryDigest'
> & {
  readonly participants: readonly ZLinkAggregateParticipant[];
  readonly sealedInventoryDigest?: string;
};

/** Target boundary counts are installed in the owner commit, never a later phase write. */
export type ZLinkAggregateFence = AuthorityContract.ZLinkAggregateFence & {
  readonly relocationAttempt?: {
    readonly representative: AuthorityContract.ZLinkAuthorityKey;
    readonly generation: bigint;
  };
  readonly actorMemberships?: ReadonlyMap<string, ZLinkRelocationActorMembership>;
  readonly acceptedRequests?: ReadonlyMap<string, bigint>;
};

export interface ZLinkRelocationActorMembership {
  readonly actorSpotId: string;
  readonly actorSpotGeneration: bigint;
  readonly actorSpotKind:
    import('../../contracts').ZLinkSpotKind.Entry | import('../../contracts').ZLinkSpotKind.User;
}

export type ZLinkAuthorityScanResult =
  | { readonly kind: 'scanExpired' }
  | {
      readonly kind: 'page';
      readonly items: readonly {
        readonly key: AuthorityContract.ZLinkAuthorityKey;
        readonly snapshot: ZLinkAuthoritySnapshot;
      }[];
      readonly nextCursor?: AuthorityContract.ZLinkAuthorityScanCursor;
    };
