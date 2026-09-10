// SPDX-License-Identifier: MPL-2.0

import type { MonitorEventType, MonitorSocket } from '../eventing';
import type { RoutingId } from '../core';
import type { ReceiveFlowState } from './socket_constants';
import type { DealerSocket } from './dealer_socket';
import type { PairSocket } from './pair_socket';
import type { PubSocket, SubSocket, XPubSocket, XSubSocket } from './pubsub_sockets';
import type { RouterSocket } from './router_socket';
import type { StreamSocket } from './stream_socket';

/** Notify the caller to drain the socket's receive surface until no data remains. */
export type ZLinkReadableHandler = () => void;

/** Base contract shared by every socket type: lifetime, binding, TLS, and monitoring. */
export interface Socket {
  /**
   * Bind to a local transport address (for example `tcp://*:5555`) to accept
   * connections there.
   */
  bind(endpoint: string): void;
  /** Stop accepting connections at `endpoint`, a previously bound address. */
  unbind(endpoint: string): void;
  /** Close the socket and release its native resources. */
  close(): void;
  /**
   * Register the socket's receive readiness handler. Replaces the previous
   * handler and keeps the Node event loop active until the socket is closed.
   * Drain with DontWait until no data remains; notifications do not count messages.
   * A watch failure is reported by the next receive, never as a handler argument.
   */
  setReadableHandler(handler: ZLinkReadableHandler): void;
  /**
   * Open a monitor reporting the selected lifecycle `events`. The caller owns
   * the returned monitor and must close it.
   */
  monitorOpen(
    events?: readonly MonitorEventType[],
    monitorHwmBytes?: bigint
  ): MonitorSocket;
  /**
   * Configure this socket as a TLS server. Apply before binding. `cert`/`key`
   * are PEM paths; `requireClientCert` requires mutual TLS.
   */
  setTlsServer(cert: string, key: string, requireClientCert?: boolean): void;
  /**
   * Configure this socket as a TLS client. Apply before connecting. `ca` is the
   * CA bundle path, `hostname` the expected server hostname, and `trustSystem`
   * also trusts the system CA store.
   */
  setTlsClient(ca: string, hostname: string, trustSystem?: boolean): void;
  /**
   * Set this socket's local receive-flow state. Only DEALER and ROUTER
   * sockets support this; PAIR, the PUB/SUB family, and STREAM report
   * `ConfigResult.NotSupported`. Setting the same state again succeeds
   * (idempotent). This does not expose flow frames or any PAUSE-bypass send
   * path; observe flow transitions only through the existing monitor and
   * snapshot surfaces.
   */
  setReceiveFlowState(state: ReceiveFlowState): void;
}

/** A socket that can also initiate outbound connections, not just bind. */
export interface ConnectableSocket extends Socket {
  /**
   * Connect to a remote transport address. Connection is asynchronous; this
   * returns before the peer is reachable.
   */
  connect(endpoint: string): void;
  /** Disconnect the connection previously established to `endpoint`. */
  disconnect(endpoint: string): void;
  /** Disconnect the peer identified by `routingId` rather than by address. */
  disconnectRid(routingId: RoutingId): void;
}

export type BaseSocket =
  | PairSocket
  | PubSocket
  | SubSocket
  | DealerSocket
  | RouterSocket
  | XPubSocket
  | XSubSocket
  | StreamSocket;
