// SPDX-License-Identifier: MPL-2.0

import type { MonitorSocket } from '../eventing';
import type { RoutingId } from '../core';
import type { SocketMonitorHandler } from '../messaging';
import type { DealerSocket } from './dealer_socket';
import type { PairSocket } from './pair_socket';
import type { PubSocket, SubSocket, XPubSocket, XSubSocket } from './pubsub_sockets';
import type { RouterSocket } from './router_socket';
import type { StreamSocket } from './stream_socket';

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
   * Open a monitor reporting the selected lifecycle `events`. The caller owns
   * the returned monitor and must close it.
   */
  monitorOpen(events?: number, handler?: SocketMonitorHandler): MonitorSocket;
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
