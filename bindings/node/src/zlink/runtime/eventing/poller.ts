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
import { MonitorSocket } from './monitor_socket';
import type { RuntimeBaseSocket as BaseSocket } from '../sockets';
import { completionOwnerOf, type CompletionOwner } from '../messaging/completion_owner';
import type { Pollable } from '../../contracts/eventing/poller';

type BasePollable = BaseSocket | MonitorSocket;

interface PollRegistration {
  source: Pollable;
  slot: number;
  nativeToken: number;
}

interface SocketRegistration extends PollRegistration {
  source: BasePollable;
  events: number;
  handle: unknown;
  owner: CompletionOwner | null;
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

function validateMonitorEvents(events: number): number {
  if (events !== PollEventFlag.PollIn) {
    throw createError(
      'config',
      22,
      'socket monitor poll events must contain PollIn only'
    );
  }
  return events;
}

export class Poller {
  private _native: unknown | null;
  private readonly _socketRegistrations = new Map<BasePollable, SocketRegistration>();
  private readonly _timerRegistrations = new Map<Timer, PollRegistration>();
  private readonly _fdRegistrations = new Map<number, PollRegistration>();
  private readonly _registrationsByToken = new Map<number, PollRegistration>();
  private _nextRegistrationToken = 1;

  constructor() { this._native = requireNative().pollerNew(); }

  add(socket: BasePollable, events: readonly PollEventFlagValue[], slot: number): void;
  add(timer: Timer, slot: number): void;
  add(item: BasePollable | Timer, eventsOrSlot?: readonly PollEventFlagValue[] | number, slot?: number): void {
    if (item instanceof Timer) {
      this.addTimerInternal(item, eventsOrSlot as number);
      return;
    }
    const events = flagsToMask(eventsOrSlot as readonly PollEventFlagValue[]);
    this.addSocketInternal(
      item,
      item instanceof MonitorSocket ? validateMonitorEvents(events) : events,
      slot as number
    );
  }

  modify(socket: BasePollable, events: readonly PollEventFlagValue[]): void {
    const mask = flagsToMask(events);
    this.modifySocketInternal(
      socket,
      socket instanceof MonitorSocket ? validateMonitorEvents(mask) : mask
    );
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
    const nativeToken = this.allocateRegistrationToken();
    try {
      requireNative().pollerAddFd(this._native, normalizedFd, BigInt(nativeToken), mask);
      const registration: PollRegistration = {
        source: normalizedFd,
        slot: normalizedSlot,
        nativeToken,
      };
      this._fdRegistrations.set(normalizedFd, registration);
      this._registrationsByToken.set(nativeToken, registration);
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
    const registration = this._fdRegistrations.get(normalizedFd);
    this._fdRegistrations.delete(normalizedFd);
    if (registration) this._registrationsByToken.delete(registration.nativeToken);
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
      source: Pollable;
      sourceKind: number;
      slot: number;
      revents: number;
      fd: number;
    }> = [];
    for (let index = 0; index < nativeCount; index += 1) {
      let revents = events.revents(index);
      const sourceKind = events.sourceKind(index);
      const nativeSlot = events.slot(index);
      const registration = this._registrationsByToken.get(nativeSlot);
      if (!registration) continue;
      const socketRegistration = typeof registration.source === 'number'
        ? undefined
        : this._socketRegistrations.get(registration.source as BasePollable);
      if (socketRegistration?.owner
          && (revents & (PollEventFlag.PollOut | PollEventFlag.PollCompletion)) !== 0) {
        const completionReady = socketRegistration.transferred
          && (revents & PollEventFlag.PollCompletion) !== 0;
        const managedWritableReady = (revents & PollEventFlag.PollOut) !== 0
          && socketRegistration.owner.hasManagedWritableWait();
        const processed = completionReady || managedWritableReady
          ? socketRegistration.owner.drain(this)
          : 0;
        if (processed === 0) {
          revents &= ~PollEventFlag.PollCompletion;
        }
      }
      if (revents === 0) continue;
      processedEvents.push({
        source: registration.source,
        sourceKind,
        slot: registration.slot,
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
        if (registration.transferred) registration.owner!.transferToRuntime(this);
      }
    }
    this._socketRegistrations.clear();
    this._timerRegistrations.clear();
    this._fdRegistrations.clear();
    this._registrationsByToken.clear();
  }

  close(): void { this.destroy(); }

  private addSocketInternal(socket: BasePollable, events: number, slot: number): void {
    const normalizedSlot = validateSlot(slot);
    const owner = socket instanceof MonitorSocket ? null : completionOwnerOf(socket);
    const transferred = owner !== null
      && (events & PollEventFlag.PollCompletion) !== 0;
    const nativeToken = this.allocateRegistrationToken();
    const handle = getNativeHandle(socket);
    const acquired = transferred ? acquireCompletionOwner(owner!, this) : false;
    try {
      requireNative().pollerAdd(
        this._native,
        handle,
        BigInt(nativeToken),
        events | 0
      );
      const registration: SocketRegistration = {
        source: socket,
        events: events | 0,
        handle,
        slot: normalizedSlot,
        nativeToken,
        owner,
        transferred,
      };
      this._socketRegistrations.set(socket, registration);
      this._registrationsByToken.set(nativeToken, registration);
    } catch (error) {
      if (acquired) owner!.transferToRuntime(this);
      throw createError('config', readErrno(), nativeErrorMessage(error, 'poller socket add failed'));
    }
  }

  private modifySocketInternal(socket: BasePollable, events: number): void {
    const handle = getNativeHandle(socket);
    const registration = this._socketRegistrations.get(socket);
    const shouldTransfer = registration?.owner != null
      && (events & PollEventFlag.PollCompletion) !== 0;
    const acquired = registration?.owner && shouldTransfer && !registration.transferred
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
        registration.owner!.transferToRuntime(this);
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
    if (registration?.transferred) registration.owner!.transferToRuntime(this);
    this._socketRegistrations.delete(socket);
    if (registration) this._registrationsByToken.delete(registration.nativeToken);
    return true;
  }

  private allocateRegistrationToken(): number {
    if (this._registrationsByToken.size >= Number.MAX_SAFE_INTEGER) {
      throw new RangeError('poller registration token space exhausted');
    }
    for (;;) {
      const token = this._nextRegistrationToken;
      this._nextRegistrationToken = token === Number.MAX_SAFE_INTEGER
        ? 1
        : token + 1;
      if (!this._registrationsByToken.has(token)) return token;
    }
  }

  private addTimerInternal(timer: Timer, slot: number): void {
    const normalizedSlot = validateSlot(slot);
    const nativeToken = this.allocateRegistrationToken();
    try {
      requireNative().pollerAddTimer(this._native, getNativeHandle(timer), BigInt(nativeToken));
      const registration: PollRegistration = {
        source: timer,
        slot: normalizedSlot,
        nativeToken,
      };
      this._timerRegistrations.set(timer, registration);
      this._registrationsByToken.set(nativeToken, registration);
    } catch (error) {
      throw createError('config', readErrno(), nativeErrorMessage(error, 'poller timer add failed'));
    }
  }

  private removeTimerInternal(timer: Timer): boolean {
    configCall('poller timer remove failed', () => {
      requireNative().pollerRemoveTimer(this._native, getNativeHandle(timer));
    });
    const registration = this._timerRegistrations.get(timer);
    this._timerRegistrations.delete(timer);
    if (registration) this._registrationsByToken.delete(registration.nativeToken);
    return true;
  }
}

export { Poller as RuntimePoller };
