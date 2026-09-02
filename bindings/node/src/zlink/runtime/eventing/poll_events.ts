// SPDX-License-Identifier: MPL-2.0

import type { PollEventFlagValue } from '../../contracts/sockets/socket_constants';
import { closeCall } from '../errors/native_errors';
import { NativeHandle } from '../handles/native_handle';
import { requireNative } from '../native/native';

export class PollEvents extends NativeHandle {
  private _readyCount = 0;
  private _nativeReadyCount = 0;
  private _synthetic: ReadonlyArray<{
    sourceKind: number;
    slot: number;
    revents: number;
    fd: number;
  }> = [];
  readonly capacity: number;

  constructor(capacity: number) {
    if (!Number.isInteger(capacity) || capacity <= 0) {
      throw new RangeError('capacity must be a positive integer');
    }
    super(requireNative().pollEventsNew(capacity));
    this.capacity = capacity;
  }

  get readyCount(): number { return this._readyCount; }

  sourceKind(index: number): number {
    this.checkReadyIndex(index);
    if (index >= this._nativeReadyCount) {
      return this._synthetic[index - this._nativeReadyCount].sourceKind;
    }
    return requireNative().pollEventsSourceKind(this._native, index | 0) as number;
  }

  slot(index: number): number {
    this.checkReadyIndex(index);
    if (index >= this._nativeReadyCount) {
      return this._synthetic[index - this._nativeReadyCount].slot;
    }
    return requireNative().pollEventsSlot(this._native, index | 0) as number;
  }

  revents(index: number): number {
    this.checkReadyIndex(index);
    if (index >= this._nativeReadyCount) {
      return this._synthetic[index - this._nativeReadyCount].revents;
    }
    return requireNative().pollEventsRevents(this._native, index | 0) as number;
  }

  fd(index: number): number {
    this.checkReadyIndex(index);
    if (index >= this._nativeReadyCount) {
      return this._synthetic[index - this._nativeReadyCount].fd;
    }
    return Number(requireNative().pollEventsFd(this._native, index | 0));
  }

  hasEvent(index: number, event: PollEventFlagValue): boolean {
    return (this.revents(index) & (event as number)) !== 0;
  }

  /** @internal */
  markReadyCount(count: number): void {
    if (count < 0 || count > this.capacity) {
      throw new RangeError('ready count out of range');
    }
    this._readyCount = count;
    this._nativeReadyCount = count;
    this._synthetic = [];
  }

  /** @internal */
  markCombined(
    nativeCount: number,
    entries: ReadonlyArray<{
      sourceKind: number;
      slot: number;
      revents: number;
      fd: number;
    }>
  ): void {
    if (nativeCount < 0 || nativeCount + entries.length > this.capacity) {
      throw new RangeError('ready count out of range');
    }
    this._nativeReadyCount = nativeCount;
    this._synthetic = entries;
    this._readyCount = nativeCount + entries.length;
  }

  close(): void {
    if (!this._native) return;
    closeCall('poll events close failed', () => {
      requireNative().pollEventsDestroy(this._native);
    });
    this._native = null;
    this._readyCount = 0;
    this._nativeReadyCount = 0;
    this._synthetic = [];
  }

  private checkReadyIndex(index: number): void {
    if (!Number.isInteger(index) || index < 0 || index >= this._readyCount) {
      throw new RangeError(`ready index ${index}`);
    }
  }
}

export { PollEvents as RuntimePollEvents };
