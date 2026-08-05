// SPDX-License-Identifier: MPL-2.0

import { RoutingId } from '../core/routing_id';
import { MessagePartsEnvelope } from './message_parts_envelope';

/**
 * A received publish: its topic and message parts. Owns its parts until closed.
 */
export class TopicMessage extends MessagePartsEnvelope {
  /** The source routing id, or null when the receive path provides none. */
  routingId: RoutingId | null;
  /** The topic the message was published under. */
  topic: string;

  /** Create an empty reusable envelope for use with `subscribe`. */
  constructor() {
    super();
    this.routingId = null;
    this.topic = '';
  }
}
