// SPDX-License-Identifier: MPL-2.0

import type {
  MonitorEventValueRaw,
  MonitorStatusRaw,
  NativeHandle
} from './binding_types';

export interface EventingNativeBinding {
  atomicCounterDec: (counter: NativeHandle) => number;
  atomicCounterDestroy: (counter: NativeHandle) => void;
  atomicCounterInc: (counter: NativeHandle) => number;
  atomicCounterNew: () => NativeHandle;
  atomicCounterSet: (counter: NativeHandle, value: number) => void;
  atomicCounterValue: (counter: NativeHandle) => number;
  monitorClose: (monitor: NativeHandle) => void;
  monitorHandler: (
    monitor: NativeHandle,
    handler: (event: MonitorEventValueRaw) => void
  ) => void;
  monitorRecv: (monitor: NativeHandle) => MonitorEventValueRaw;
  monitorRecvNoWait: (monitor: NativeHandle) => MonitorEventValueRaw | null;
  monitorStatus: (monitor: NativeHandle) => MonitorStatusRaw;
  pollEventsDestroy: (events: NativeHandle) => void;
  pollEventsFd: (events: NativeHandle, index: number) => number;
  pollEventsNew: (capacity: number) => NativeHandle;
  pollEventsRevents: (events: NativeHandle, index: number) => number;
  pollEventsSlot: (events: NativeHandle, index: number) => number;
  pollEventsSourceKind: (events: NativeHandle, index: number) => number;
  pollerAdd: (
    poller: NativeHandle,
    handle: NativeHandle,
    slot: bigint | null,
    events: number
  ) => void;
  pollerAddFd: (
    poller: NativeHandle,
    fd: number,
    slot: bigint,
    events: number
  ) => void;
  pollerAddTimer: (poller: NativeHandle, timer: NativeHandle, slot: bigint) => void;
  pollerDestroy: (poller: NativeHandle) => void;
  pollerModify: (poller: NativeHandle, handle: NativeHandle, events: number) => void;
  pollerModifyFd: (poller: NativeHandle, fd: number, events: number) => void;
  pollerNew: () => NativeHandle;
  pollerRemove: (poller: NativeHandle, handle: NativeHandle) => void;
  pollerRemoveFd: (poller: NativeHandle, fd: number) => void;
  pollerRemoveTimer: (poller: NativeHandle, timer: NativeHandle) => void;
  pollerSize: (poller: NativeHandle) => number;
  pollerWait: (poller: NativeHandle, timeoutMs: number) => number;
  pollerWaitInto: (
    poller: NativeHandle,
    events: NativeHandle,
    capacity: number,
    timeoutMs: number
  ) => number;
  stopwatchIntermediate: (watch: NativeHandle) => number;
  stopwatchStart: () => NativeHandle;
  stopwatchStop: (watch: NativeHandle) => number;
  timerDestroy: (timer: NativeHandle) => void;
  timerHandler: (
    timer: NativeHandle,
    handler: (fireCount: bigint) => void
  ) => void;
  timerNew: () => NativeHandle;
  timerRecv: (timer: NativeHandle, flags: number) => bigint | null;
  timerStart: (timer: NativeHandle, intervalNs: bigint, repeatCount: bigint) => void;
  timerStop: (timer: NativeHandle) => void;
}
