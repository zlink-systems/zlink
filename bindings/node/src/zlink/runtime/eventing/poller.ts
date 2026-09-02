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
import { getNativeHandle } from '../handles/native_handle';
import { requireNative } from '../native/native';
import { flagsToMask } from '../sockets/socket_options';
import { PollEvents } from './poll_events';
import { Timer } from './timer';
import type { RuntimeBaseSocket as BaseSocket } from '../sockets';
import { completionOwnerOf, type CompletionOwner } from '../messaging/completion_owner';

type BasePollable = BaseSocket;

function validateSlot(slot: number): number {
  if (!Number.isSafeInteger(slot) || slot < 0) {
    throw new RangeError('slot must be a non-negative safe integer');
  }
  return slot;
}

export class Poller {
  private _native: unknown | null;
  private readonly _socketRegistrations = new Map<
    BasePollable,
    { events: number; handle: unknown; slot: number; owner: CompletionOwner; transferred: boolean }
  >();

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
    let nativeCount: number;
    try {
      nativeCount = requireNative().pollerWaitInto(
        this._native,
        getNativeHandle(events),
        events.capacity | 0,
        timeoutMs | 0
      ) as number;
    } catch (error) {
      if (isWouldBlock()) {
        events.markCombined(0, []);
        return 0;
      }
      throw recvNativeError(error, RecvFlags.None, 'poller wait failed');
    }

    events.markReadyCount(nativeCount | 0);
    const processedEvents: Array<{
      sourceKind: number;
      slot: number;
      revents: number;
      fd: number;
    }> = [];
    for (let index = 0; index < nativeCount; index += 1) {
      let revents = events.revents(index);
      const slot = events.slot(index);
      if ((revents & PollEventFlag.PollCompletion) !== 0) {
        let processed = 0;
        for (const registration of this._socketRegistrations.values()) {
          if (registration.slot === slot && registration.transferred) {
            processed += registration.owner.drain();
          }
        }
        if (processed === 0) revents &= ~PollEventFlag.PollCompletion;
      }
      if (revents === 0) continue;
      processedEvents.push({
        sourceKind: events.sourceKind(index),
        slot,
        revents,
        fd: events.fd(index),
      });
    }
    events.markCombined(0, processedEvents);
    return processedEvents.length;
  }

  destroy(): void {
    if (this._native) {
      for (const registration of this._socketRegistrations.values()) {
        if (registration.transferred) registration.owner.transferToRuntime(this);
      }
      closeCall('poller close failed', () => {
        requireNative().pollerDestroy(this._native);
      });
      this._native = null;
    }
    this._socketRegistrations.clear();
  }

  close(): void { this.destroy(); }

  private addSocketInternal(socket: BasePollable, events: number, slot: number): void {
    const normalizedSlot = validateSlot(slot);
    const owner = completionOwnerOf(socket);
    const transferred = (events & PollEventFlag.PollCompletion) !== 0;
    try {
      if (transferred) owner.transferToPublic(this);
      requireNative().pollerAdd(this._native, getNativeHandle(socket), BigInt(normalizedSlot), events | 0);
      const handle = getNativeHandle(socket);
      this._socketRegistrations.set(socket, {
        events: events | 0,
        handle,
        slot: normalizedSlot,
        owner,
        transferred,
      });
    } catch (error) {
      if (transferred) owner.transferToRuntime(this);
      throw createError('config', readErrno(), nativeErrorMessage(error, 'poller socket add failed'));
    }
  }

  private modifySocketInternal(socket: BasePollable, events: number): void {
    const handle = getNativeHandle(socket);
    const registration = this._socketRegistrations.get(socket);
    const shouldTransfer = (events & PollEventFlag.PollCompletion) !== 0;
    const acquired = !!registration && shouldTransfer && !registration.transferred;
    if (acquired) {
      registration.owner.transferToPublic(this);
    }
    try {
      configCall('poller socket modify failed', () => {
        requireNative().pollerModify(this._native, handle, events | 0);
      });
    } catch (error) {
      if (acquired) registration!.owner.transferToRuntime(this);
      throw error;
    }
    if (registration) {
      if (!shouldTransfer && registration.transferred) {
        registration.owner.transferToRuntime(this);
      }
      registration.events = events | 0;
      registration.transferred = shouldTransfer;
    }
  }

  private removeSocketInternal(socket: BasePollable): boolean {
    configCall('poller socket remove failed', () => {
      requireNative().pollerRemove(this._native, getNativeHandle(socket));
    });
    const registration = this._socketRegistrations.get(socket);
    if (registration?.transferred) registration.owner.transferToRuntime(this);
    this._socketRegistrations.delete(socket);
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
