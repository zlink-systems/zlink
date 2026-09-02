// SPDX-License-Identifier: MPL-2.0

import type { Message, MessageLike } from './message';

/** Builder stage that accepts one message part and returns the next stage. */
export interface PartBuilder<TNext> {
  /** Add one message part; it is consumed on a successful submit. */
  message(message: MessageLike): TNext;
}

/** Builder stage that sets an operation timeout and returns the next stage. */
export interface Timeoutable<TNext> {
  /** Set how long the operation waits before timing out. */
  timeout(timeoutMs: number): TNext;
}

/** Builds a Core-completion-driven multipart send. */
export interface SendOperation extends PartBuilder<SendSubmitOperation> {}

/** Accepts further parts and the synchronous or Promise terminal. */
export interface SendSubmitOperation
  extends PartBuilder<SendSubmitOperation> {
  /** Resolve when Core reports send completion; reject on timeout or terminal failure. */
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
  /** Submit the request and return the reply parts, which the caller owns. */
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
