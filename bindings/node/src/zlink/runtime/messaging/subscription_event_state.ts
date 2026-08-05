// SPDX-License-Identifier: MPL-2.0

import { SubscriptionEvent } from '../../contracts/messaging/subscription_event';
import { RoutingId } from '../../contracts/core/routing_id';

interface SubscriptionEventState {
  routingId: RoutingId | null;
  topic: string;
  subscribed: boolean;
}

export function createSubscriptionEvent(
  topic: string,
  subscribed: boolean,
  routingId: RoutingId | null = null
): SubscriptionEvent {
  const event = new SubscriptionEvent();
  replaceSubscriptionEvent(event, topic, subscribed, routingId);
  return event;
}

export function replaceSubscriptionEvent(
  target: SubscriptionEvent,
  topic: string,
  subscribed: boolean,
  routingId: RoutingId | null = null
): void {
  const state = target as unknown as SubscriptionEventState;
  state.routingId = routingId;
  state.topic = topic;
  state.subscribed = subscribed === true;
}
