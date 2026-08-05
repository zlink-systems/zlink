import type { ZLinkInstanceSpot, ZLinkInstanceSpotContext } from '@zlink-systems/framework';

class OrderWorkflowSpot implements ZLinkInstanceSpot {
  readonly context!: ZLinkInstanceSpotContext;

}

export { OrderWorkflowSpot };
