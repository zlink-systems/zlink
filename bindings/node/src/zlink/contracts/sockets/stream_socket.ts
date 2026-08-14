// SPDX-License-Identifier: MPL-2.0

import type { RoutingId } from '../core';
import type { Received } from '../messaging';
import type { RoutedSendOperation, SendOperation } from '../messaging';
import type { StreamPacketHandler } from '../messaging';
import type { RecvFlags } from './socket_constants';
import type { StreamSocketOptions } from './socket_options';
import type { Socket } from './socket';

/**
 * STREAM socket: exchanges framed packets with raw TCP peers addressed by
 * routing id.
 */
export interface StreamSocket extends Socket {
  /** The STREAM-specific typed options facade. */
  readonly options: StreamSocketOptions;
  /**
   * Begin an exact-target managed send. `submit()` resolves only after Core
   * accepts the record for the selected `(RID, pairId, generation)`.
   */
  send(routingId: RoutingId): RoutedSendOperation;
  /** Begin an explicit immediate DONTWAIT-capable send. */
  trySend(routingId: RoutingId): SendOperation;
  /** Receive a message into `result`; false when `RecvFlags.DontWait` is set and none is available. */
  recv(result: Received, flags?: RecvFlags): boolean;
  /**
   * Receive into `result` for a Framework backend while retaining dequeued
   * Core HWM credit. Successful reuse or `result.close()` returns the credit;
   * a native finalizer is the fallback if the result becomes unreachable.
   */
  recvRetained(result: Received, flags?: RecvFlags): boolean;
  /**
   * Register the handler invoked for each inbound framed packet; the handler
   * owns the messages it receives (see {@link StreamPacketHandler}) and runs on
   * a background dispatch thread.
   */
  setPacketHandler(handler: StreamPacketHandler): void;
  /**
   * Register a callback invoked when the socket can accept more sends after
   * back-pressure. The callback runs on a background dispatch thread.
   */
  setSendReadyHandler(handler: () => void): void;
  /**
   * Set the routing id that identifies this socket to its peers. Apply before
   * connecting so peers observe it from the first packet.
   */
  setRoutingId(routingId: RoutingId): void;
  /** Return the routing id that identifies this socket to its peers. */
  getRoutingId(): RoutingId;
  /** Disconnect the peer identified by `routingId`. */
  disconnectRid(routingId: RoutingId): void;
  /** Disconnect only the transport pair identified by a monitor event. */
  disconnectTransportPair(transportPairId: bigint, transportPairGeneration: bigint): void;
}
