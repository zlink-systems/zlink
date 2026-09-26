// SPDX-License-Identifier: MPL-2.0

import type { RoutingId } from '../core';
import type { Received, ReplyToken } from '../messaging';
import type {
  ReplyOperation,
  RequestOperation,
  SendOperation,
} from '../messaging';
import type { RecvFlags } from './socket_constants';
import type { RouterSocketOptions } from './socket_options';
import type { ConnectableSocket } from './socket';

/**
 * ROUTER socket: routes messages to peers addressed by routing id, the server
 * side of asynchronous request/reply.
 */
/**
 * One row of the ROUTER selected-route snapshot. Core selects one application
 * route per routing id; `routeGeneration` is a nonzero opaque token that
 * changes whenever that selection changes and is compared only for equality.
 */
export interface RouterRoute {
  readonly routingId: RoutingId;
  readonly routeGeneration: bigint;
}

export interface RouterSocket extends ConnectableSocket {
  /** The ROUTER-specific typed options facade. */
  readonly options: RouterSocketOptions;
  /** Begin a send addressed to `routingId`; parts are consumed on a successful submit. */
  send(routingId: RoutingId): SendOperation;
  /**
   * Receive a routed message into `result`; false when `RecvFlags.DontWait` is
   * set and no message is available.
   */
  recv(result: Received, flags?: RecvFlags): boolean;
  /**
   * Set the routing id that identifies this ROUTER to its peers. Apply before
   * connecting so peers observe it from the first message.
   */
  setRoutingId(routingId: RoutingId): void;
  /** Return the routing id that identifies this ROUTER to its peers. */
  getRoutingId(): RoutingId;
  /** Begin a request to peer `peerRid`; parts are consumed on submit and a reply is awaited. */
  request(peerRid: RoutingId): RequestOperation;
  /** Begin a reply using the opaque token returned by this ROUTER's request receive. */
  reply(peerRid: RoutingId, token: ReplyToken): ReplyOperation;
  /**
   * Return the selected route of every routing id atomically. A routing id
   * without a row has no selected route. A successful snapshot clears
   * `PollEventFlag.PollRoute` readiness; one observer per socket calls it.
   */
  routesSnapshot(): RouterRoute[];
}
