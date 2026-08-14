// SPDX-License-Identifier: MPL-2.0

import { TopicMessage } from '../../contracts/messaging/topic_message';
import {
  Message,
  acquireMessageWrapper,
  refillMessageWrapper
} from '../../contracts/messaging/message';
import { RoutingId } from '../../contracts/core/routing_id';
import {
  closeMessageParts,
  freezeMessageParts,
  freezeOwnedMessageParts
} from './message_parts_state';
import { replaceMessagePartsEnvelopeRetainedCredit } from '../../contracts/messaging/message_parts_envelope';

interface TopicMessageState {
  parts: Message[];
  routingId: RoutingId | null;
  topic: string;
  _reusableSinglePart: Message | null;
  _reusableSinglePartSlots: Message[][];
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
    try {
      closeMessageParts(state.parts);
    } finally {
      replaceMessagePartsEnvelopeRetainedCredit(target, null);
    }
  } else {
    replaceMessagePartsEnvelopeRetainedCredit(target, null);
  }
  const candidate = state._reusableSinglePart;
  if (candidate && !parts.includes(candidate)) {
    candidate.close();
  }
  state._reusableSinglePart = null;
  state._reusableSinglePartSlots = [];
  state.parts = Object.isFrozen(parts) ? parts : freezeOwnedMessageParts(parts);
  state.routingId = routingId;
  state.topic = topic;
}

/**
 * Replace a caller-provided result from the common owned-Buffer receive path.
 * The previously visible single part becomes the next receive candidate only
 * after the new payload has been adopted, so no public part is reused early.
 */
export function replaceTopicMessageOwnedSinglePart(
  target: TopicMessage,
  topic: string,
  data: Buffer,
  routingId: RoutingId | null = null
): void {
  const state = target as unknown as TopicMessageState;
  let next = state._reusableSinglePart;
  if (next === null) {
    next = acquireMessageWrapper(data);
  } else {
    refillMessageWrapper(next, data);
  }

  const previous = state.parts.length === 1 ? state.parts[0] : null;
  if (state.parts !== undefined) {
    for (const part of state.parts) {
      if (part !== next && part !== previous) {
        part.close();
      }
    }
  }
  state.parts = frozenSinglePartSlot(state, next);
  state._reusableSinglePart = previous === next ? null : previous;
  state.routingId = routingId;
  state.topic = topic;
}

function frozenSinglePartSlot(state: TopicMessageState, part: Message): Message[] {
  for (const slot of state._reusableSinglePartSlots) {
    if (slot[0] === part) {
      return slot;
    }
  }
  const slot = freezeOwnedMessageParts([part]);
  if (state._reusableSinglePartSlots.length < 2) {
    state._reusableSinglePartSlots.push(slot);
  }
  return slot;
}
