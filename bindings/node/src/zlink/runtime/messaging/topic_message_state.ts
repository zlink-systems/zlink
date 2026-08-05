// SPDX-License-Identifier: MPL-2.0

import { TopicMessage } from '../../contracts/messaging/topic_message';
import { Message } from '../../contracts/messaging/message';
import { RoutingId } from '../../contracts/core/routing_id';
import {
  closeMessageParts,
  freezeMessageParts,
  freezeOwnedMessageParts
} from './message_parts_state';

interface TopicMessageState {
  parts: Message[];
  routingId: RoutingId | null;
  topic: string;
}

export function createTopicMessage(
  topic: string,
  parts: readonly Message[],
  routingId: RoutingId | null = null
): TopicMessage {
  const message = new TopicMessage();
  replaceTopicMessage(message, topic, freezeMessageParts(parts), routingId);
  return message;
}

export function replaceTopicMessage(
  target: TopicMessage,
  topic: string,
  parts: Message[],
  routingId: RoutingId | null = null
): void {
  const state = target as unknown as TopicMessageState;
  // Caller-provided receive storage is a hot path. Replacing the envelope
  // must release the previously owned payload immediately; otherwise reused
  // TopicMessage instances retain external Buffer storage until GC and PUBSUB
  // subscriber benchmarks regress in both RSS and throughput.
  if (state.parts !== parts) {
    closeMessageParts(state.parts);
  }
  state.parts = Object.isFrozen(parts) ? parts : freezeOwnedMessageParts(parts);
  state.routingId = routingId;
  state.topic = topic;
}
