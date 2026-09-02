// SPDX-License-Identifier: MPL-2.0

import type { RoutingId } from '../core';
import type { Received, SendOperation, StreamPacket } from '../messaging';
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
   * accepts the record for the routing id captured by this builder.
   */
  send(routingId: RoutingId): SendOperation;
  /** Receive a message into `result`; false when `RecvFlags.DontWait` is set and none is available. */
  recv(result: Received, flags?: RecvFlags): boolean;
  /** Receive one packet into reusable storage; false for non-blocking no-data. */
  recvPacket(result: StreamPacket, flags?: RecvFlags): boolean;
  /**
   * Set the routing id that identifies this socket to its peers. Apply before
   * connecting so peers observe it from the first packet.
   */
  setRoutingId(routingId: RoutingId): void;
  /** Return the routing id that identifies this socket to its peers. */
  getRoutingId(): RoutingId;
  /** Disconnect the peer identified by `routingId`. */
  disconnectRid(routingId: RoutingId): void;
}
