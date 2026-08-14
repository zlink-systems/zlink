// SPDX-License-Identifier: MPL-2.0

import type { SendFlags } from '../sockets/socket_constants';
import type { Message, MessageLike } from './message';

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

/** Builds a binding-owned asynchronous admission wait for one send record. */
export interface AsyncSendOperation extends PartBuilder<AsyncSendSubmitOperation> {}

/** Async send terminal: Core, rather than a Framework retry queue, owns readiness. */
export interface AsyncSendSubmitOperation
  extends PartBuilder<AsyncSendSubmitOperation>, Timeoutable<AsyncSendSubmitOperation> {
  /** Resolve after Core accepts the complete record; reject on timeout or terminal failure. */
  submit(): Promise<void>;
}

/** Builds a DEALER/ROUTER send whose terminal waits asynchronously for Core admission. */
export interface RoutedSendOperation extends PartBuilder<RoutedSendSubmitOperation> {}

/** Accepts further routed parts and exposes the sole managed terminal. */
export interface RoutedSendSubmitOperation
  extends PartBuilder<RoutedSendSubmitOperation>, Timeoutable<RoutedSendSubmitOperation> {
  /** Resolve after Core accepts the complete record; reject on terminal failure. */
  submit(): Promise<void>;
}

/** Builds a request: add the request parts, then submit and await a reply. */
export interface RequestOperation extends PartBuilder<RequestSubmitOperation> {}

/** Accepts further parts, timeout, and the sole managed terminal of a request. */
export interface RequestSubmitOperation
  extends PartBuilder<RequestSubmitOperation>, Timeoutable<RequestSubmitOperation> {
  /** Submit the request and return the reply parts, which the caller owns. */
  submit(): Promise<Message[]>;
}

/** Builds a reply to a received request: add the reply parts, then submit. */
export interface ReplyOperation extends PartBuilder<ReplySubmitOperation> {}

/** Accepts further parts, flags, and the terminal submit of a reply. */
export interface ReplySubmitOperation
  extends PartBuilder<ReplySubmitOperation>, Flaggable<ReplySubmitOperation> {
  /** Submit the accumulated reply parts. */
  submit(): void;
}
