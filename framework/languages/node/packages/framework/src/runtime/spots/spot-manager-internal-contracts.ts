import type { RoutingId, ZLinkSpotCreateState } from '../../contracts';

export interface ZLinkLocalSpotCreateResult {
  readonly spotId: RoutingId;
  readonly state: ZLinkSpotCreateState;
  readonly reply?: unknown;
}
