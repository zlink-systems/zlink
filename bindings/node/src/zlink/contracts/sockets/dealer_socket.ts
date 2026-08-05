// SPDX-License-Identifier: MPL-2.0

import type { RoutingId } from '../core';
import type { RequestOperation } from '../messaging';
import type { PairSocket } from './pair_socket';
import type { DealerSocketOptions } from './socket_options';

/**
 * DEALER socket: load-balances sends across its connected peers and can issue
 * routed requests.
 */
export interface DealerSocket extends PairSocket {
  /** The DEALER-specific typed options facade. */
  readonly options: DealerSocketOptions;
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
