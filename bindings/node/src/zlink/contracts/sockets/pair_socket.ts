// SPDX-License-Identifier: MPL-2.0

import type { Received } from '../messaging';
import type { SendOperation } from '../messaging';
import type { RecvFlags } from './socket_constants';
import type { CommonSocketOptions } from './socket_options';
import type { ConnectableSocket } from './socket';

/** PAIR socket: an exclusive one-to-one peering with no routing. */
export interface PairSocket extends ConnectableSocket {
  /** The typed options facade common to all socket types. */
  readonly options: CommonSocketOptions;
  /**
   * Begin a multipart send: add parts on the returned builder, then submit.
   * Parts are consumed on a successful submit.
   */
  send(): SendOperation;
  /**
   * Receive a message into `result`; return true on success, or false when
   * `RecvFlags.DontWait` is set and no message is available.
   */
  recv(result: Received, flags?: RecvFlags): boolean;
  /**
   * Receive into `result` for a Framework backend while retaining dequeued
   * Core HWM credit. Successful reuse or `result.close()` returns the credit;
   * a native finalizer is the fallback if the result becomes unreachable.
   */
  recvRetained(result: Received, flags?: RecvFlags): boolean;
  /**
   * Register a callback invoked when the socket can accept more sends after
   * back-pressure. The callback runs on a background dispatch thread.
   */
  setSendReadyHandler(handler: () => void): void;
}
