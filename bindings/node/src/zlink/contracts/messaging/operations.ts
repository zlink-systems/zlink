// SPDX-License-Identifier: MPL-2.0

import type { RequestResult } from '../errors/errors';
import type { SendFlags } from '../sockets/socket_constants';
import type { Message, MessageLike } from './message';

/** Invoked with a request result and its reply parts; the callback owns the parts and must close them. */
export type RequestCallback = (result: RequestResult, parts: readonly Message[]) => void;
/** Invoked with a request result and its reply parts; the callback owns the parts. */
export type ReplyHandler = (result: RequestResult, parts: Message[]) => void;

/** Builder stage that accepts one message part and returns the next stage. */
export interface PartBuilder<TNext> {
  /** Add one message part; it is consumed on a successful submit. */
  message(message: MessageLike): TNext;
}

/** Builder stage that sets send flags and returns the next stage. */
export interface Flaggable<TNext> {
  /** Set the send flags applied at submit time. */
  flags(flags: SendFlags): TNext;
}

/** Builder stage that sets an operation timeout and returns the next stage. */
export interface Timeoutable<TNext> {
  /** Set how long the operation waits before timing out. */
  timeout(timeoutMs: number): TNext;
}

/**
 * Builds a multipart send: add parts, then submit.
 *
 * Submitting consumes the added {@link Message} parts: on a successful submit
 * each part's payload is moved into the transport and the managed message is
 * left empty, so a part must not be reused after a successful submit. The
 * request and reply builders share this same consume-on-submit ownership model.
 */
export interface SendOperation extends PartBuilder<SendSubmitOperation> {}

/** Accepts further parts, flags, and the terminal submit of a send. */
export interface SendSubmitOperation
  extends PartBuilder<SendSubmitOperation>, Flaggable<SendSubmitOperation> {
  /**
   * Submit the accumulated parts. Return true when queued, and false only when
   * `SendFlags.DontWait` is set and the send would have blocked (back-pressure).
   */
  submit(): boolean;
}

/** Builds a request: add the request parts, then submit and await a reply. */
export interface RequestOperation extends PartBuilder<RequestSubmitOperation> {}

/** Accepts further parts, timeout, flags, and the terminal submit of a request. */
export interface RequestSubmitOperation
  extends PartBuilder<RequestSubmitOperation>, Timeoutable<RequestSubmitOperation> {
  /** Set the send flags and narrow the builder to callback submission. */
  flags(flags: SendFlags): RequestCallbackSubmitOperation;
  /** Submit the request and return the reply parts, which the caller owns. */
  submit(): Promise<Message[]>;
  /** Submit the request; the result and reply parts are delivered to `callback`. Returns false under DontWait back-pressure. */
  submit(callback: RequestCallback): boolean;
}

/** Callback-submission stage of a request (reached after setting flags). */
export interface RequestCallbackSubmitOperation
  extends PartBuilder<RequestCallbackSubmitOperation>,
    Timeoutable<RequestCallbackSubmitOperation>,
    Flaggable<RequestCallbackSubmitOperation> {
  /** Submit the request; the result and reply parts are delivered to `callback`. */
  submit(callback: RequestCallback): boolean;
}

/** Builds a reply to a received request: add the reply parts, then submit. */
export interface ReplyOperation extends PartBuilder<ReplySubmitOperation> {}

/** Accepts further parts, flags, and the terminal submit of a reply. */
export interface ReplySubmitOperation
  extends PartBuilder<ReplySubmitOperation>, Flaggable<ReplySubmitOperation> {
  /** Submit the accumulated reply parts. */
  submit(): void;
}
