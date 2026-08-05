// SPDX-License-Identifier: MPL-2.0

import { NativeHandle } from '../handles/native_handle';
import { requireNative } from '../native/native';
import {
  closeCall,
  configCall,
  handlerCall,
  recvNativeError
} from '../errors/native_errors';
import { RecvFlags } from '../../contracts/sockets/socket_constants';
import {
  MonitorEvent,
  type MonitorStatus,
} from '../../contracts/eventing';
import type { SocketMonitorHandler } from '../../contracts/messaging';
import { createMonitorEvent } from './monitor_event_state';
import { materializeMonitorStatus } from './monitor_status';
import type { MonitorEventValueRaw, MonitorStatusRaw } from './monitor_raw';

export class MonitorSocket extends NativeHandle {
  static readonly ignoreHandler: SocketMonitorHandler = () => {};

  constructor(native: unknown) {
    super(native);
  }

  recv(flags: RecvFlags = RecvFlags.None): MonitorEvent | null {
    try {
      if ((flags | 0) & (RecvFlags.DontWait | 0)) {
        const raw = requireNative().monitorRecvNoWait(this._native) as MonitorEventValueRaw | null;
        return raw ? createMonitorEvent(raw) : null;
      }
      return createMonitorEvent(requireNative().monitorRecv(this._native) as MonitorEventValueRaw);
    } catch (error) {
      throw recvNativeError(error, flags, 'monitor recv failed');
    }
  }

  onEvent(handler: SocketMonitorHandler): void {
    handlerCall('monitor handler registration failed', () => {
      requireNative().monitorHandler(this._native, (event: MonitorEventValueRaw) => {
        handler(createMonitorEvent(event));
      });
    });
  }

  status(): MonitorStatus {
    return materializeMonitorStatus(configCall('monitor status failed', () =>
      requireNative().monitorStatus(this._native) as MonitorStatusRaw
    ));
  }

  close(): void {
    if (this._native) {
      closeCall('monitor close failed', () => {
        requireNative().monitorClose(this._native);
      });
      this._native = null;
    }
  }
}
