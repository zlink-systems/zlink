// SPDX-License-Identifier: MPL-2.0

import {
  PollEventFlag,
  RecvFlags,
  type PollEventFlagValue,
} from '../../contracts/sockets/socket_constants';
import { createError } from '../errors/error_mapping';
import {
  closeCall,
  configCall,
  isWouldBlock,
  nativeErrorMessage,
  readErrno,
  recvNativeError,
} from '../errors/native_errors';
import {
  acquireExternalRequestProgress,
  releaseExternalRequestProgress,
} from '../messaging/request_progress';
import { getNativeHandle } from '../handles/native_handle';
import { requireNative } from '../native/native';
import { flagsToMask } from '../sockets/socket_options';
import { PollEvents } from './poll_events';
import { Timer } from './timer';
import type { RuntimeBaseSocket as BaseSocket } from '../sockets';

type BasePollable = BaseSocket;

function validateSlot(slot: number): number {
  if (!Number.isSafeInteger(slot) || slot < 0) {
    throw new RangeError('slot must be a non-negative safe integer');
  }
  return slot;
}

export class Poller {
  private _native: unknown | null;
  private readonly _externalProgressHandles = new Set<unknown>();

  constructor() { this._native = requireNative().pollerNew(); }

  add(socket: BasePollable, events: readonly PollEventFlagValue[], slot: number): void;
  add(timer: Timer, slot: number): void;
  add(item: BasePollable | Timer, eventsOrSlot?: readonly PollEventFlagValue[] | number, slot?: number): void {
    if (item instanceof Timer) {
      this.addTimerInternal(item, eventsOrSlot as number);
      return;
    }
    this.addSocketInternal(item, flagsToMask(eventsOrSlot as readonly PollEventFlagValue[]), slot as number);
  }

  modify(socket: BasePollable, events: readonly PollEventFlagValue[]): void {
    this.modifySocketInternal(socket, flagsToMask(events));
  }

  remove(socket: BasePollable): boolean;
  remove(timer: Timer): boolean;
  remove(item: BasePollable | Timer): boolean {
    if (item instanceof Timer) {
      return this.removeTimerInternal(item);
    }
    return this.removeSocketInternal(item);
  }

  addFd(fd: number, events: readonly PollEventFlagValue[], slot: number): void {
    const mask = flagsToMask(events);
    const normalizedFd = fd | 0;
    const normalizedSlot = validateSlot(slot);
    try {
      requireNative().pollerAddFd(this._native, normalizedFd, BigInt(normalizedSlot), mask);
    } catch (error) {
      throw createError('config', readErrno(), nativeErrorMessage(error, 'poller fd add failed'));
    }
  }

  modifyFd(fd: number, events: readonly PollEventFlagValue[]): void {
    const mask = flagsToMask(events);
    const normalizedFd = fd | 0;
    configCall('poller fd modify failed', () => {
      requireNative().pollerModifyFd(this._native, normalizedFd, mask);
    });
  }

  removeFd(fd: number): boolean {
    const normalizedFd = fd | 0;
    configCall('poller fd remove failed', () => {
      requireNative().pollerRemoveFd(this._native, normalizedFd);
    });
    return true;
  }

  get size(): number {
    return configCall('poller size failed', () =>
      requireNative().pollerSize(this._native) as number
    );
  }

  wait(events: PollEvents, timeoutMs: number): number {
    try {
      const count = requireNative().pollerWaitInto(
        this._native,
        getNativeHandle(events),
        events.capacity | 0,
        timeoutMs | 0
      ) as number;
      events.markReadyCount(count | 0);
      return count | 0;
    } catch (error) {
      if (isWouldBlock()) {
        events.markReadyCount(0);
        return 0;
      }
      throw recvNativeError(error, RecvFlags.None, 'poller wait failed');
    }
  }

  destroy(): void {
    if (this._native) {
      closeCall('poller close failed', () => {
        requireNative().pollerDestroy(this._native);
      });
      this._native = null;
    }
    for (const handle of this._externalProgressHandles) {
      releaseExternalRequestProgress(handle);
    }
    this._externalProgressHandles.clear();
  }

  close(): void { this.destroy(); }

  private addSocketInternal(socket: BasePollable, events: number, slot: number): void {
    const normalizedSlot = validateSlot(slot);
    try {
      requireNative().pollerAdd(this._native, getNativeHandle(socket), BigInt(normalizedSlot), events | 0);
      if ((events & PollEventFlag.PollCompletion) !== 0) {
        const handle = getNativeHandle(socket);
        acquireExternalRequestProgress(handle);
        this._externalProgressHandles.add(handle);
      }
    } catch (error) {
      throw createError('config', readErrno(), nativeErrorMessage(error, 'poller socket add failed'));
    }
  }

  private modifySocketInternal(socket: BasePollable, events: number): void {
    const handle = getNativeHandle(socket);
    configCall('poller socket modify failed', () => {
      requireNative().pollerModify(this._native, handle, events | 0);
    });
    const hasExternal = this._externalProgressHandles.has(handle);
    const wantsExternal = (events & PollEventFlag.PollCompletion) !== 0;
    if (wantsExternal && !hasExternal) {
      acquireExternalRequestProgress(handle);
      this._externalProgressHandles.add(handle);
    } else if (!wantsExternal && hasExternal) {
      releaseExternalRequestProgress(handle);
      this._externalProgressHandles.delete(handle);
    }
  }

  private removeSocketInternal(socket: BasePollable): boolean {
    configCall('poller socket remove failed', () => {
      requireNative().pollerRemove(this._native, getNativeHandle(socket));
    });
    const handle = getNativeHandle(socket);
    if (this._externalProgressHandles.delete(handle)) {
      releaseExternalRequestProgress(handle);
    }
    return true;
  }

  private addTimerInternal(timer: Timer, slot: number): void {
    const normalizedSlot = validateSlot(slot);
    try {
      requireNative().pollerAddTimer(this._native, getNativeHandle(timer), BigInt(normalizedSlot));
    } catch (error) {
      throw createError('config', readErrno(), nativeErrorMessage(error, 'poller timer add failed'));
    }
  }

  private removeTimerInternal(timer: Timer): boolean {
    configCall('poller timer remove failed', () => {
      requireNative().pollerRemoveTimer(this._native, getNativeHandle(timer));
    });
    return true;
  }
}

export { Poller as RuntimePoller };
