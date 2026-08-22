// SPDX-License-Identifier: MPL-2.0

import {
  MonitorEvent,
  type MonitorEventType
} from '../../contracts/eventing/monitor';
import { RoutingId } from '../../contracts/core';
import type { MonitorEventValueRaw } from './monitor_raw';

interface MonitorEventState {
  event: MonitorEventType;
  value: bigint;
  routingId: RoutingId | null;
  localAddr: string;
  remoteAddr: string;
  connectionId: bigint;
  transportPairId: bigint;
  transportPairGeneration: bigint;
  transportLane: number;
  flags: number;
}

export function createMonitorEvent(raw: MonitorEventValueRaw): MonitorEvent {
  const event = Object.create(MonitorEvent.prototype) as MonitorEvent;
  const state = event as unknown as MonitorEventState;
  state.event = raw.event as MonitorEventType;
  state.value = raw.value ?? 0n;
  state.routingId = raw.routingId && raw.routingId.length > 0 ? RoutingId.from(raw.routingId) : null;
  state.localAddr = raw.localAddr ?? raw.local ?? '';
  state.remoteAddr = raw.remoteAddr ?? raw.remote ?? '';
  state.connectionId = raw.connectionId ?? 0n;
  state.transportPairId = raw.transportPairId ?? 0n;
  state.transportPairGeneration = raw.transportPairGeneration ?? 0n;
  state.transportLane = raw.transportLane ?? 0;
  state.flags = raw.flags ?? 0;
  return event;
}
