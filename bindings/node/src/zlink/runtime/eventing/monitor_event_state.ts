// SPDX-License-Identifier: MPL-2.0

import {
  MonitorEvent,
  type MonitorEventType
} from '../../contracts/eventing/monitor';
import { RoutingId } from '../../contracts/core';
import type { MonitorEventValueRaw } from './monitor_raw';

interface MonitorEventState {
  event: MonitorEventType;
  value: number;
  routingId: RoutingId | null;
  localAddr: string;
  remoteAddr: string;
}

export function createMonitorEvent(raw: MonitorEventValueRaw): MonitorEvent {
  const event = Object.create(MonitorEvent.prototype) as MonitorEvent;
  const state = event as unknown as MonitorEventState;
  state.event = raw.event as MonitorEventType;
  state.value = raw.value;
  state.routingId = raw.routingId && raw.routingId.length > 0 ? RoutingId.from(raw.routingId) : null;
  state.localAddr = raw.localAddr ?? raw.local ?? '';
  state.remoteAddr = raw.remoteAddr ?? raw.remote ?? '';
  return event;
}
