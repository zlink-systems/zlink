export * from '../../contracts/Locations';
import type { ZLinkObjectReserveRequest as ReserveRequest } from '../../contracts/Locations/Authority';
import type { ZLinkRelocationConfiguration } from '../../contracts/Configuration/RegistrationTypes';

export interface ZLinkObjectReserveRequest extends ReserveRequest {
  readonly actorRelocationPolicy?: ZLinkRelocationConfiguration<unknown>['kind'];
}
export type ZLinkEndedOwnerReleaseIntent = Pick<
  ZLinkObjectReserveRequest,
  'key' | 'actorRelocationPolicy'
> & { readonly intent: Pick<ZLinkObjectReserveRequest['intent'], 'stableType'> };
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
