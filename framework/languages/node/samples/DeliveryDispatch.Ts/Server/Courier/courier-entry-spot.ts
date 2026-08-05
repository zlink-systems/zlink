import type {
  ZLinkEntrySpot,
  ZLinkEntrySpotContext
} from '@zlink-systems/framework';
import type { CourierActor } from './courier-actor';

class CourierEntrySpot implements ZLinkEntrySpot<CourierActor> {
  readonly context!: ZLinkEntrySpotContext<CourierActor>;
  async onJoinedActor(_actor: CourierActor): Promise<void> {}
  async onLeaveActor(_actor: CourierActor): Promise<void> {}
  async onDisconnectActor(_actor: CourierActor): Promise<void> {}
}

export { CourierEntrySpot };
