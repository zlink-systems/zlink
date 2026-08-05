import type { RoutingId, ZLinkSpotCreateState } from '../../contracts';

export interface ZLinkLocalSpotCreateResult {
  readonly spotId: RoutingId;
  readonly state: ZLinkSpotCreateState;
  readonly reply?: unknown;
  readonly publication?: {
    publish(): void;
    abort(): void;
  };
}
