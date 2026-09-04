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
const POLLER_SOURCE_SOCKET = 1;

interface SocketRegistration {
  events: number;
  handle: unknown;
  slot: number;
  nativeToken: number;
  owner: CompletionOwner;
  transferred: boolean;
}

function validateSlot(slot: number): number {
  if (!Number.isSafeInteger(slot) || slot < 0) {
    throw new RangeError('slot must be a non-negative safe integer');
  }
  return slot;
}

function acquireCompletionOwner(owner: CompletionOwner, poller: Poller): boolean {
  try {
    return owner.transferToPublic(poller);
  } catch (error) {
    throw createError(
      'config',
      16,
      nativeErrorMessage(error, 'poller completion ownership transfer failed')
    );
  }
}

export class Poller {
  private _native: unknown | null;
  private readonly _socketRegistrations = new Map<BasePollable, SocketRegistration>();
  private readonly _socketRegistrationsByToken = new Map<number, SocketRegistration>();
  private _nextSocketRegistrationToken = 1;

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
      const sourceKind = events.sourceKind(index);
      const nativeSlot = events.slot(index);
      const registration = sourceKind === POLLER_SOURCE_SOCKET
        ? this._socketRegistrationsByToken.get(nativeSlot)
        : undefined;
      if (sourceKind === POLLER_SOURCE_SOCKET && !registration) continue;
      const slot = registration?.slot ?? nativeSlot;
      if (registration
          && (revents & (PollEventFlag.PollOut | PollEventFlag.PollCompletion)) !== 0) {
        const completionReady = registration.transferred
          && (revents & PollEventFlag.PollCompletion) !== 0;
        const managedWritableReady = (revents & PollEventFlag.PollOut) !== 0
          && registration.owner.hasManagedWritableWait();
        const processed = completionReady || managedWritableReady
          ? registration.owner.drain(this)
          : 0;
        if (processed === 0) {
          revents &= ~PollEventFlag.PollCompletion;
        }
      }
      if (revents === 0) continue;
      processedEvents.push({
        sourceKind,
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
      closeCall('poller close failed', () => {
        requireNative().pollerDestroy(this._native);
      });
      this._native = null;
      for (const registration of this._socketRegistrations.values()) {
        if (registration.transferred) registration.owner.transferToRuntime(this);
      }
    }
    this._socketRegistrations.clear();
    this._socketRegistrationsByToken.clear();
  }

  close(): void { this.destroy(); }

  private addSocketInternal(socket: BasePollable, events: number, slot: number): void {
    const normalizedSlot = validateSlot(slot);
    const owner = completionOwnerOf(socket);
    const transferred = (events & PollEventFlag.PollCompletion) !== 0;
    const nativeToken = this.allocateSocketRegistrationToken();
    const handle = getNativeHandle(socket);
    const acquired = transferred ? acquireCompletionOwner(owner, this) : false;
    try {
      requireNative().pollerAdd(
        this._native,
        handle,
        BigInt(nativeToken),
        events | 0
      );
      const registration: SocketRegistration = {
        events: events | 0,
        handle,
        slot: normalizedSlot,
        nativeToken,
        owner,
        transferred,
      };
      this._socketRegistrations.set(socket, registration);
      this._socketRegistrationsByToken.set(nativeToken, registration);
    } catch (error) {
      if (acquired) owner.transferToRuntime(this);
      throw createError('config', readErrno(), nativeErrorMessage(error, 'poller socket add failed'));
    }
  }

  private modifySocketInternal(socket: BasePollable, events: number): void {
    const handle = getNativeHandle(socket);
    const registration = this._socketRegistrations.get(socket);
    const shouldTransfer = (events & PollEventFlag.PollCompletion) !== 0;
    const acquired = registration && shouldTransfer && !registration.transferred
      ? acquireCompletionOwner(registration.owner, this)
      : false;
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
    if (registration) this._socketRegistrationsByToken.delete(registration.nativeToken);
    return true;
  }

  private allocateSocketRegistrationToken(): number {
    if (this._socketRegistrationsByToken.size >= Number.MAX_SAFE_INTEGER) {
      throw new RangeError('socket registration token space exhausted');
    }
    for (;;) {
      const token = this._nextSocketRegistrationToken;
      this._nextSocketRegistrationToken = token === Number.MAX_SAFE_INTEGER
        ? 1
        : token + 1;
      if (!this._socketRegistrationsByToken.has(token)) return token;
    }
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
