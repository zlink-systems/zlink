// SPDX-License-Identifier: MPL-2.0

import type { RoutingId } from '../core';
import type {
  Received,
  RequestOperation,
  RoutedSendOperation,
} from '../messaging';
import type { RecvFlags } from './socket_constants';
import type { DealerSocketOptions } from './socket_options';
import type { ConnectableSocket } from './socket';

/**
 * DEALER socket: load-balances sends across its connected peers and can issue
 * routed requests.
 */
export interface DealerSocket extends ConnectableSocket {
  /** The DEALER-specific typed options facade. */
  readonly options: DealerSocketOptions;
  /** Begin a managed send; `submit()` resolves after Core accepts the record. */
  send(): RoutedSendOperation;
  /** Receive a message into `result`; false for non-blocking no-data. */
  recv(result: Received, flags?: RecvFlags): boolean;
  /** Receive while retaining the origin Core HWM credit for Framework use. */
  recvRetained(result: Received, flags?: RecvFlags): boolean;
  /**
   * Set the routing id that identifies this DEALER to its peers. Apply before
   * connecting so peers observe it from the first message.
   */
  setRoutingId(routingId: RoutingId): void;
  /** Return the routing id that identifies this DEALER to its peers. */
  getRoutingId(): RoutingId;
  /**
   * Begin a request: add parts on the returned builder, then submit and await a
   * reply. Parts are consumed on a successful submit.
   */
  request(): RequestOperation;
}
