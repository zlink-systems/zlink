// SPDX-License-Identifier: MPL-2.0

import { RuntimeContext as Context } from '../core/context';
import { getNativeHandle, NativeHandle } from '../handles/native_handle';
import { requireNative } from '../native/native';
import {
  bindCall,
  closeCall,
  configCall,
  connectCall,
  lastError
} from '../errors/native_errors';
import { validateCString } from '../options/validation';
import { RoutingId } from '../../contracts';
import { type MonitorEventType } from '../../contracts/eventing';
import { SOCKET_MONITOR_EVENT_ALL, type ReceiveFlowState } from '../../contracts/sockets/socket_constants';
import { normalizeRoutingId } from '../core/routing_id';
import { MonitorSocket } from '../eventing/monitor_socket';
import { validateUInt64 } from '../options/byte_values';
import {
  installCompletionOwner,
  releaseCompletionOwner,
} from '../messaging/completion_owner';

export class SocketBase extends NativeHandle {
  constructor(ctx: Context, type: number) {
    super(requireNative().socketNew(getNativeHandle(ctx), type));
    if (!this._native) {
      throw lastError('config', 'socket creation failed');
    }
    installCompletionOwner(this, this._native);
  }

  bind(endpoint: string): void {
    const normalizedEndpoint = validateCString(endpoint, 'endpoint');
    bindCall('socket bind failed', () => {
      requireNative().socketBind(getNativeHandle(this), normalizedEndpoint);
    });
  }

  unbind(endpoint: string): void {
    const normalizedEndpoint = validateCString(endpoint, 'endpoint');
    connectCall('socket unbind failed', () => {
      requireNative().socketUnbind(getNativeHandle(this), normalizedEndpoint);
    });
  }

  setTlsServer(cert: string, key: string, requireClientCert: boolean = false): void {
    const normalizedCert = validateCString(cert, 'cert', Number.MAX_SAFE_INTEGER);
    const normalizedKey = validateCString(key, 'key', Number.MAX_SAFE_INTEGER);
    configCall('socket TLS server configuration failed', () => {
      requireNative().socketSetTlsServer(
        getNativeHandle(this),
        normalizedCert,
        normalizedKey,
        requireClientCert ? 1 : 0
      );
    });
  }

  setTlsClient(ca: string, hostname: string, trustSystem: boolean = false): void {
    const normalizedCa = validateCString(ca, 'ca', Number.MAX_SAFE_INTEGER);
    const normalizedHostname = validateCString(hostname, 'hostname', Number.MAX_SAFE_INTEGER);
    configCall('socket TLS client configuration failed', () => {
      requireNative().socketSetTlsClient(
        getNativeHandle(this),
        normalizedCa,
        normalizedHostname,
        trustSystem ? 1 : 0
      );
    });
  }

  setReceiveFlowState(state: ReceiveFlowState): void {
    configCall('socket receive-flow-state configuration failed', () => {
      requireNative().socketSetReceiveFlowState(getNativeHandle(this), state | 0);
    });
  }

  /** @internal */
  setSockOptRaw(option: number, value: Buffer | number): void {
    const buf = typeof value === 'number' ? Buffer.from([value & 0xff, 0, 0, 0]) : value;
    configCall('socket option set failed', () => {
      requireNative().socketSetOpt(getNativeHandle(this), option | 0, buf);
    });
  }

  /** @internal */
  getSockOptRaw(option: number): Buffer {
    return configCall('socket option get failed', () =>
      requireNative().socketGetOpt(getNativeHandle(this), option | 0) as Buffer
    );
  }

  monitorOpen(
    events?: readonly MonitorEventType[],
    monitorHwmBytes: bigint = 0n
  ): MonitorSocket {
    const mask = events === undefined
      ? SOCKET_MONITOR_EVENT_ALL
      : events.reduce((current, event) => current | (event | 0), 0);
    const exactMonitorHwmBytes = validateUInt64(
      monitorHwmBytes,
      'monitorHwmBytes'
    );
    return new MonitorSocket(configCall('monitor open failed', () =>
      requireNative().monitorOpen(
        getNativeHandle(this),
        mask | 0,
        exactMonitorHwmBytes
      )
    ));
  }

  close(): void {
    if (!this._native) return;
    releaseCompletionOwner(this);
    closeCall('socket close failed', () => {
      requireNative().socketClose(this._native);
    });
    this._native = null;
  }
}

export class ConnectableSocket extends SocketBase {
  connect(endpoint: string): void {
    const normalizedEndpoint = validateCString(endpoint, 'endpoint');
    connectCall('socket connect failed', () => {
      requireNative().socketConnect(getNativeHandle(this), normalizedEndpoint);
    });
  }

  disconnect(endpoint: string): void {
    const normalizedEndpoint = validateCString(endpoint, 'endpoint');
    connectCall('socket disconnect failed', () => {
      requireNative().socketDisconnect(getNativeHandle(this), normalizedEndpoint);
    });
  }

  disconnectRid(routingId: RoutingId): void {
    const normalizedRoutingId = normalizeRoutingId(routingId);
    connectCall('socket disconnect by routing id failed', () => {
      requireNative().socketDisconnectRid(
        getNativeHandle(this),
        normalizedRoutingId
      );
    });
  }

}
