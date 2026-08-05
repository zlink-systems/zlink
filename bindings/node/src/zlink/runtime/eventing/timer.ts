// SPDX-License-Identifier: MPL-2.0

import { RecvResult } from '../../contracts/errors/errors';
import type { TimerHandler } from '../../contracts/eventing';
import { RecvFlags } from '../../contracts/sockets/socket_constants';
import {
  closeCall,
  configCall,
  handlerCall,
  recvNativeError,
} from '../errors/native_errors';
import { requireNative } from '../native/native';
import { NativeHandle } from '../handles/native_handle';

export class Timer extends NativeHandle {
  constructor() {
    super(requireNative().timerNew());
  }

  start(intervalNs: bigint, repeatCount: bigint): void {
    configCall('timer start failed', () => {
      requireNative().timerStart(this._native, intervalNs, repeatCount);
    });
  }

  stop(): void {
    configCall('timer stop failed', () => {
      requireNative().timerStop(this._native);
    });
  }

  recv(): bigint | null {
    const flags = RecvFlags.None;
    try {
      return requireNative().timerRecv(this._native, flags | 0) as bigint | null;
    } catch (error) {
      const recvError = recvNativeError(error, flags, 'timer recv failed');
      if (recvError.result === RecvResult.NoData) return null;
      throw recvError;
    }
  }

  onFire(handler: TimerHandler): void {
    handlerCall('timer handler registration failed', () => {
      requireNative().timerHandler(this._native, (fireCount: bigint) => handler(this, fireCount));
    });
  }

  close(): void {
    if (this._native) {
      closeCall('timer close failed', () => {
        requireNative().timerDestroy(this._native);
      });
      this._native = null;
    }
  }
}
