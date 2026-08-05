// SPDX-License-Identifier: MPL-2.0

import { SubmitError, SubmitResult } from '../errors/errors';
import type { ReplyOperation, SendOperation } from './operations';
import { RoutingId } from '../core/routing_id';
import { MessagePartsEnvelope } from './message_parts_envelope';

interface ReplyContext {
  beginReply(): ReplyOperation;
}

interface SendContext {
  beginSend(): SendOperation;
}

function invalidReplyContextError(): SubmitError {
  const error = new SubmitError(SubmitResult.InvalidState, 0);
  error.message = 'reply is only valid for request-reply receive contexts';
  return error;
}

function invalidSendContextError(): SubmitError {
  const error = new SubmitError(SubmitResult.InvalidState, 0);
  error.message = 'send is only valid for received routed message contexts';
  return error;
}

/**
 * A received message envelope: its routing metadata and message parts, plus an
 * optional reply/send context.
 *
 * Owns its parts until closed. Reuse one instance across `recv` calls to avoid
 * a per-receive allocation.
 */
export class Received extends MessagePartsEnvelope {
  /** The source routing id, or null when the receive path provides none. */
  routingId: RoutingId | null;
  /** The request sequence, present when this envelope can be replied to. */
  requestSeq: bigint | null;
  private _replyContext: ReplyContext | null;
  private _sendContext: SendContext | null;

  /** Create an empty reusable envelope for use with `recv`. */
  constructor(...args: never[]) {
    if (args.length > 0) {
      throw new TypeError('Received reusable envelopes are created without constructor arguments');
    }
    super();
    this.routingId = null;
    this.requestSeq = null;
    this._replyContext = null;
    this._sendContext = null;
  }

  /**
   * Begin a reply to this request: add parts on the returned builder, then
   * submit. Parts are consumed on a successful submit. Throws when the envelope
   * is not replyable (has no request sequence).
   */
  reply(): ReplyOperation {
    if (!this.requestSeq || !this._replyContext) {
      throw invalidReplyContextError();
    }
    return this._replyContext.beginReply();
  }

  /**
   * Begin a send addressed to this envelope's source route: add parts, then
   * submit. Parts are consumed on a successful submit. Throws when the envelope
   * carries no send context.
   */
  send(): SendOperation {
    if (!this._sendContext) {
      throw invalidSendContextError();
    }
    return this._sendContext.beginSend();
  }
}
