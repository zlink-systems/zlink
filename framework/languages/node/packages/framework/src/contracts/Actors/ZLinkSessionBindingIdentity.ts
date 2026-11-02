import type { RoutingId } from '../Common';

export interface ZLinkSessionBindingIdentity {
  readonly sessionOwnerNodeRid: RoutingId;
  readonly sessionOwnerNodeGeneration: bigint;
  readonly bindingGeneration: bigint;
}
