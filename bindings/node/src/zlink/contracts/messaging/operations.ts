// SPDX-License-Identifier: MPL-2.0

import type { SendFlags } from '../sockets/socket_constants';
import type { Message, MessageLike } from './message';

/** Builder stage that accepts one message part and returns the next stage. */
export interface PartBuilder<TNext> {
  /** Add one message part; it is consumed on a successful submit. */
  message(message: MessageLike): TNext;
}

/** Builder stage that sets send flags for an immediate raw operation. */
export interface Flaggable<TNext> {
  /** Set the send flags applied at submit time. */
  flags(flags: SendFlags): TNext;
}

/** Builder stage that sets an operation timeout and returns the next stage. */
export interface Timeoutable<TNext> {
  /** Set how long the operation waits before timing out. */
  timeout(timeoutMs: number): TNext;
}

/** Builds a Core-completion-driven multipart send. */
export interface SendOperation extends PartBuilder<SendSubmitOperation> {}

/** Accepts further parts, an optional per-operation timeout, and submit. */
export interface SendSubmitOperation
  extends PartBuilder<SendSubmitOperation>, Timeoutable<SendSubmitOperation> {
  /** Resolve when Core reports send completion; reject on timeout or terminal failure. */
  submit(): Promise<void>;
}

/** Builds a DEALER/ROUTER/STREAM routed send. */
export interface RoutedSendOperation extends PartBuilder<RoutedSendSubmitOperation> {}

/** Accepts further routed parts, a per-operation timeout, and submit. */
export interface RoutedSendSubmitOperation
  extends PartBuilder<RoutedSendSubmitOperation>, Timeoutable<RoutedSendSubmitOperation> {
  /** Resolve after Core accepts the complete record; reject on terminal failure. */
  submit(): Promise<void>;
}

/** Immediate raw send retained for STREAM relay/try-send surfaces. */
export interface ImmediateSendOperation extends PartBuilder<ImmediateSendSubmitOperation> {}

/** Immediate raw send accepts flags and reports whether Core admitted it. */
export interface ImmediateSendSubmitOperation
  extends PartBuilder<ImmediateSendSubmitOperation>, Flaggable<ImmediateSendSubmitOperation> {
  submit(): boolean;
}

/** Builds a synchronous lossy PUB/XPUB publish. */
export interface PublishOperation extends PartBuilder<PublishSubmitOperation> {}

/** PUB/XPUB submit is void-or-throw; Core owns lossy/NODROP behavior. */
export interface PublishSubmitOperation extends PartBuilder<PublishSubmitOperation> {
  submit(): void;
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
