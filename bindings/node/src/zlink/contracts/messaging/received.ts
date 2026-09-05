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

const replyTokenOwnerIds = new WeakMap<object, number>();
let nextReplyTokenOwnerId = 1;
let makeReplyToken!: (owner: object, value: bigint) => ReplyToken;
let tokenOwnerMatches!: (token: ReplyToken, owner: object) => boolean;
let tokenNativeValue!: (token: ReplyToken) => bigint;

/** Opaque capability required to reply to one ROUTER request. */
export class ReplyToken {
  readonly #owner: object;
  readonly #value: bigint;

  private constructor(secret: symbol, owner: object, value: bigint) {
    if (secret !== replyTokenSecret || value === 0n) {
      throw new TypeError('ReplyToken values are created by ROUTER request receive');
    }
    this.#owner = owner;
    this.#value = value;
    Object.freeze(this);
  }

  static {
    makeReplyToken = (owner: object, value: bigint): ReplyToken =>
      new ReplyToken(replyTokenSecret, owner, value);
    // Keep the capability in its private fields only. Package-private
    // readers use the same brand check instead of duplicating every token
    // in a WeakMap with a second owner/value record.
    tokenOwnerMatches = (token, owner) =>
      typeof token === 'object' && token !== null
      && #owner in token && token.#owner === owner;
    tokenNativeValue = (token) => {
      if (typeof token !== 'object' || token === null || !(#value in token)) {
        throw new TypeError('invalid ReplyToken');
      }
      return token.#value;
    };
  }

  equals(other: ReplyToken): boolean {
    return other instanceof ReplyToken
      && this.#owner === other.#owner
      && this.#value === other.#value;
  }

  hashCode(): number {
    let ownerId = replyTokenOwnerIds.get(this.#owner);
    if (ownerId === undefined) {
      ownerId = nextReplyTokenOwnerId++;
      replyTokenOwnerIds.set(this.#owner, ownerId);
    }
    const valueHash = Number((this.#value ^ (this.#value >> 32n)) & 0xffffffffn);
    return (Math.imul(ownerId, 0x9e3779b1) ^ valueHash) | 0;
  }

  toString(): 'ReplyToken' { return 'ReplyToken'; }

}

const replyTokenSecret = Symbol('ReplyToken');

/** @internal Package-private receive factory; not exported from the package root. */
export function createReplyToken(owner: object, value: bigint): ReplyToken {
  return makeReplyToken(owner, value);
}

/** @internal */
export function replyTokenOwnerMatches(token: ReplyToken, owner: object): boolean {
  return tokenOwnerMatches(token, owner);
}

/** @internal */
export function replyTokenNativeValue(token: ReplyToken): bigint {
  return tokenNativeValue(token);
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
  /** Opaque reply capability, present only for a ROUTER REQUEST receive. */
  replyToken: ReplyToken | null;
  private _replyContext: ReplyContext | null;
  private _sendContext: SendContext | null;

  /** Create an empty reusable envelope for use with `recv`. */
  constructor(...args: never[]) {
    if (args.length > 0) {
      throw new TypeError('Received reusable envelopes are created without constructor arguments');
    }
    super();
    this.routingId = null;
    this.replyToken = null;
    this._replyContext = null;
    this._sendContext = null;
  }

  /**
   * Begin a reply to this request: add parts on the returned builder, then
   * submit. Parts are consumed on a successful submit. Throws when the envelope
   * is not replyable (has no reply token).
   */
  reply(): ReplyOperation {
    if (!this.replyToken || !this._replyContext) {
      throw invalidReplyContextError();
    }
    return this._replyContext.beginReply();
  }

  /**
   * Begin a send addressed to this envelope's source route: add parts, then
   * submit. Message ownership transfers on immediate admission or after the
   * managed send takes its first back-pressure snapshot, before the Promise may
   * resolve. Throws when the envelope carries no send context.
   */
  send(): SendOperation {
    if (!this._sendContext) {
      throw invalidSendContextError();
    }
    return this._sendContext.beginSend();
  }
}
