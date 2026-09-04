// SPDX-License-Identifier: MPL-2.0

import type { Message, MessageLike } from './message';

/** Builder stage that accepts one message part and returns the next stage. */
export interface PartBuilder<TNext> {
  /** Add one message part; its terminal method defines when ownership transfers. */
  message(message: MessageLike): TNext;
}

/** Builder stage that sets an operation timeout and returns the next stage. */
export interface Timeoutable<TNext> {
  /** Set how long the operation waits before timing out. */
  timeout(timeoutMs: number): TNext;
}

/** Builds a multipart send with managed nonblocking back-pressure retry. */
export interface SendOperation extends PartBuilder<SendSubmitOperation> {}

/** Accepts further parts and the synchronous or Promise terminal. */
export interface SendSubmitOperation
  extends PartBuilder<SendSubmitOperation> {
  /**
   * Resolve after admission. On back-pressure the runtime waits for the exact
   * WRITABLE token and retries an owned packet snapshot. Message ownership
   * transfers on immediate admission or after the first back-pressure snapshot,
   * which can occur before this Promise resolves. Terminal failures reject.
   */
  submit(): Promise<void>;
  /** Submit synchronously with Core blocking admission semantics. */
  submit_sync(): void;
}

/** Builds a synchronous lossy PUB/XPUB publish. */
export interface PublishOperation extends PartBuilder<PublishSubmitOperation> {}

/** PUB/XPUB submit is void-or-throw; Core owns lossy/NODROP behavior. */
export interface PublishSubmitOperation extends PartBuilder<PublishSubmitOperation> {
  submit(): void;
}

/** Builds a request: add the request parts, then submit and await a reply. */
export interface RequestOperation extends PartBuilder<RequestSubmitOperation> {}

/** Accepts further parts, reply timeout, and the two request terminals. */
export interface RequestSubmitOperation
  extends PartBuilder<RequestSubmitOperation>, Timeoutable<RequestSubmitOperation> {
  /**
   * Submit the request and return the caller-owned reply parts. Backpressure
   * waits for this request's WRITABLE token before resubmitting the same packet;
   * the reply timeout starts only after admission.
   */
  submit(): Promise<Message[]>;
  submit_sync(): Message[];
}

/** Builds a reply to a received request: add the reply parts, then submit. */
export interface ReplyOperation extends PartBuilder<ReplySubmitOperation> {}

/** Accepts further parts and the flag-free synchronous reply terminal. */
export interface ReplySubmitOperation
  extends PartBuilder<ReplySubmitOperation> {
  /** Submit the accumulated reply parts. */
  submit(): void;
}
