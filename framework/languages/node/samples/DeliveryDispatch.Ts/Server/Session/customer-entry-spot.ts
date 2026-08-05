import { CustomerActor } from './customer-actor';
import type {
  ZLinkEntrySpot,
  ZLinkEntrySpotContext
} from '@zlink-systems/framework';

class CustomerEntrySpot implements ZLinkEntrySpot<CustomerActor> {
  readonly context!: ZLinkEntrySpotContext<CustomerActor>;
  async onJoinedActor(_actor: CustomerActor): Promise<void> {}
  async onLeaveActor(_actor: CustomerActor): Promise<void> {}
  async onDisconnectActor(_actor: CustomerActor): Promise<void> {}
}

export { CustomerEntrySpot };
