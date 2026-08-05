// SPDX-License-Identifier: MPL-2.0

import { RoutingId } from '../core/routing_id';

/** A subscriber's subscribe or unsubscribe as observed by an XPUB socket. */
export class SubscriptionEvent {
  /** The subscriber's routing id, or null when not known. */
  routingId: RoutingId | null;
  /** The topic that was subscribed or unsubscribed. */
  topic: string;
  /** True for a subscribe, false for an unsubscribe. */
  subscribed: boolean;

  /** Create an empty reusable event for use with `receiveSubscriptionEvent`. */
  constructor() {
    this.routingId = null;
    this.topic = '';
    this.subscribed = false;
  }
}
