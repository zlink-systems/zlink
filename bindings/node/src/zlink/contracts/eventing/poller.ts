// SPDX-License-Identifier: MPL-2.0

import type { BaseSocket } from '../sockets';
import type { PollEventFlagValue } from '../sockets/socket_constants';
import type { Timer } from './timer';

/** One ready source reported by {@link Poller.wait}. */
export interface PollEvent {
  /** Whether the ready source is a socket, file descriptor, or timer. */
  readonly sourceKind: number;
  /** The caller token supplied when the source was registered. */
  readonly slot: number;
  /** The events that fired, as a mask of poll flags. */
  readonly revents: number;
  /** The file descriptor, when the source is a raw fd. */
  readonly fd: number;
}

/** A reusable buffer of poll results filled by {@link Poller.wait}. */
export interface PollEvents {
  /** The maximum number of results the buffer can hold. */
  readonly capacity: number;
  /** The number of ready sources written by the last wait. */
  readonly readyCount: number;
  /** The source kind of the result at `index`. */
  sourceKind(index: number): number;
  /** The caller token of the result at `index`. */
  slot(index: number): number;
  /** The fired-event mask of the result at `index`. */
  revents(index: number): number;
  /** The file descriptor of the result at `index`. */
  fd(index: number): number;
  /** Whether the result at `index` includes `event`. */
  hasEvent(index: number, event: PollEventFlagValue): boolean;
  /** Release the buffer. */
  close(): void;
}

/** Multiplexes sockets, file descriptors, and timers, reporting which become ready on a single wait. */
export interface Poller {
  /** The number of registered sources. */
  readonly size: number;
  /** Register `socket` to be watched for `events`; `slot` is echoed back in the matching result. */
  add(socket: BaseSocket, events: readonly PollEventFlagValue[], slot: number): void;
  /** Register `timer`; its expirations surface as poll events tagged with `slot`. */
  add(timer: Timer, slot: number): void;
  /** Change the watched events for an already-registered socket. */
  modify(socket: BaseSocket, events: readonly PollEventFlagValue[]): void;
  /** Unregister `socket`; return true when it was registered. */
  remove(socket: BaseSocket): boolean;
  /** Unregister `timer`; return true when it was registered. */
  remove(timer: Timer): boolean;
  /** Register raw file descriptor `fd` to be watched for `events`; `slot` is echoed back. */
  addFd(fd: number, events: readonly PollEventFlagValue[], slot: number): void;
  /** Change the watched events for an already-registered file descriptor. */
  modifyFd(fd: number, events: readonly PollEventFlagValue[]): void;
  /** Unregister file descriptor `fd`; return true when it was registered. */
  removeFd(fd: number): boolean;
  /** Wait up to `timeoutMs` for sources to become ready, filling `events`; a negative timeout blocks indefinitely. Return the number of ready sources. */
  wait(events: PollEvents, timeoutMs: number): number;
  /** Close the poller and release its resources. */
  close(): void;
}
