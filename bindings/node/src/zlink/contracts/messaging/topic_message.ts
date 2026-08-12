// SPDX-License-Identifier: MPL-2.0

import { RoutingId } from '../core/routing_id';
import { Message } from './message';
import { MessagePartsEnvelope } from './message_parts_envelope';

/**
 * A received publish: its topic and message parts. Owns its parts until closed.
 */
export class TopicMessage extends MessagePartsEnvelope {
  /** The source routing id, or null when the receive path provides none. */
  routingId: RoutingId | null;
  /** The topic the message was published under. */
  topic: string;
  /** @internal Single-part receive candidate, never exposed through parts. */
  private _reusableSinglePart: Message | null;
  /** @internal Frozen single-part views paired with reusable wrappers. */
  private _reusableSinglePartSlots: Message[][];

  /** Create an empty reusable envelope for use with `subscribe`. */
  constructor() {
    super();
    this.routingId = null;
    this.topic = '';
    this._reusableSinglePart = null;
    this._reusableSinglePartSlots = [];
  }

  override close(): void {
    const candidate = this._reusableSinglePart;
    super.close();
    this._reusableSinglePart = null;
    this._reusableSinglePartSlots = [];
    if (candidate && !this.parts.includes(candidate)) {
      candidate.close();
    }
  }
}
