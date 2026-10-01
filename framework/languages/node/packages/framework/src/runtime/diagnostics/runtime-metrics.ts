export const MILLISECONDS_PER_SECOND = 1000;
export const NANOSECONDS_PER_SECOND = 1e9;

import type {
  ZLinkHostCapacityStatus,
  ZLinkMeter,
  ZLinkMeterProvider,
  ZLinkMetricAttributes
} from '../../contracts';
import { ZLinkMeters } from '../../contracts';
import { metrics as openTelemetryMetrics } from '@opentelemetry/api';

type CounterInstrument = ReturnType<ZLinkMeter['createCounter']>;
type UpDownInstrument = ReturnType<ZLinkMeter['createUpDownCounter']>;
type HistogramInstrument = ReturnType<ZLinkMeter['createHistogram']>;

interface ZLinkObservableResult {
  observe(value: number, attributes?: ZLinkMetricAttributes): void;
}

interface ZLinkObservableGauge {
  addCallback(callback: (result: ZLinkObservableResult) => void): void;
}

interface ZLinkObservableMeter extends ZLinkMeter {
  createObservableCounter?(
    name: string,
    options?: { readonly unit?: string }
  ): ZLinkObservableGauge;
  createObservableGauge?(name: string, options?: { readonly unit?: string }): ZLinkObservableGauge;
}

export interface ZLinkRuntimeMetricCapacity {
  readonly active: number;
  readonly reserved: number;
  readonly limit: number;
}

export interface ZLinkRuntimeMetricSpotTypeCapacity extends ZLinkRuntimeMetricCapacity {
  readonly spotKind: 'entry' | 'user' | 'instance';
  readonly stableType: string;
}

export interface ZLinkRuntimeMetricMeshSnapshot {
  readonly meshName: string;
  readonly source: 'manual' | 'redis' | 'manual_and_redis';
  readonly configuredPeers: number;
  readonly connectedPeers: number;
  readonly readyPeers: number;
  readonly channels: ReadonlyArray<{
    readonly channelName: string;
    readonly readyMembers: number;
  }>;
  readonly actorCapacity: ZLinkRuntimeMetricCapacity;
  readonly spotCapacity: ZLinkRuntimeMetricCapacity;
  readonly spotTypeCapacities: ReadonlyArray<ZLinkRuntimeMetricSpotTypeCapacity>;
  readonly activation: { readonly active: number; readonly limit: number };
  readonly instanceSpots: ReadonlyArray<{
    readonly instanceSpotType: string;
    readonly pendingMessages: number;
    readonly pendingBytes: number;
  }>;
}

export interface ZLinkRuntimeMetricRegistration {
  dispose(): void;
}

export interface ZLinkApplicationJobQueuePressureMetricSnapshot {
  readonly pressureState: 'running' | 'paused';
  readonly runningTransitionCount: bigint;
  readonly pausedTransitionCount: bigint;
  readonly currentPauseDurationSeconds: number;
  readonly cumulativePauseDurationSeconds: number;
  readonly flowStateConfigFailureCount: bigint;
}

export interface ZLinkRuntimeMetricOperation {
  complete(outcome: string): void;
}

export interface ZLinkRelocationMetricOperation extends ZLinkRuntimeMetricOperation {
  recordBytes(bytes: number): void;
}

export const METRIC_NAMES = Object.freeze({
  StreamConnectionsOpened: 'zlink.stream.connections.opened',
  StreamConnectionsClosed: 'zlink.stream.connections.closed',
  MeshNodeChannelSelectionFailures: 'zlink.mesh_node.channel.selection_failures',
  MeshNodeRequestTimeouts: 'zlink.mesh_node.request.timeouts',
  MeshNodeMessagesDropped: 'zlink.mesh_node.messages.dropped',
  RelocationStarted: 'zlink.relocation.started',
  RelocationCompleted: 'zlink.relocation.completed',
  RelocationCutoverTimeout: 'zlink.relocation.cutover_timeout',
  InstanceSpotActivations: 'zlink.instance_spot.activations',
  InstanceSpotClaimConflicts: 'zlink.instance_spot.claim.conflicts',
  InstanceSpotTakeovers: 'zlink.instance_spot.takeovers',
  LocationStoreErrors: 'zlink.location.store.errors',
  LocationOwnerLeaseRenewFailures: 'zlink.location.owner_lease.renew.failures',
  ObservabilityEventsOverflow: 'zlink.observability.events.overflow',
  HostRelocationBlocked: 'zlink.host.relocation.blocked',
  HostShutdownForced: 'zlink.host.shutdown.forced',
  StreamConnectionsActive: 'zlink.stream.connections.active',
  SpotCount: 'zlink.spot.count',
  ActorCount: 'zlink.actor.count',
  MeshNodeRequestsInflight: 'zlink.mesh_node.requests.inflight',
  MeshNodeRequestDuration: 'zlink.mesh_node.request.duration',
  RelocationDuration: 'zlink.relocation.duration',
  RelocationBytes: 'zlink.relocation.bytes',
  RelocationInterruption: 'zlink.relocation.interruption',
  RelocationTargetResume: 'zlink.relocation.target_resume',
  RelocationRouteConvergence: 'zlink.relocation.route_convergence',
  InstanceSpotActivationDuration: 'zlink.instance_spot.activation.duration',
  LocationOwnerLeaseRenewLateness: 'zlink.location.owner_lease.renew.lateness',
  HostRelocationDuration: 'zlink.host.relocation.duration',
  HostShutdownDuration: 'zlink.host.shutdown.duration',
  MeshNodePeersConfigured: 'zlink.mesh_node.peers.configured',
  MeshNodePeersConnected: 'zlink.mesh_node.peers.connected',
  MeshNodePeersReady: 'zlink.mesh_node.peers.ready',
  MeshNodeChannelsReadyMembers: 'zlink.mesh_node.channels.ready_members',
  ObjectCapacityActive: 'zlink.object.capacity.active',
  ObjectCapacityReserved: 'zlink.object.capacity.reserved',
  ObjectCapacityLimit: 'zlink.object.capacity.limit',
  SpotTypeCapacityActive: 'zlink.spot.type.capacity.active',
  SpotTypeCapacityReserved: 'zlink.spot.type.capacity.reserved',
  SpotTypeCapacityLimit: 'zlink.spot.type.capacity.limit',
  ObjectActivationActive: 'zlink.object.activation.active',
  ObjectActivationLimit: 'zlink.object.activation.limit',
  InstanceSpotPendingMessages: 'zlink.instance_spot.pending.messages',
  InstanceSpotPendingBytes: 'zlink.instance_spot.pending.bytes',
  HostState: 'zlink.host.state',
  HostCoreHwmEffectiveBudget: 'zlink.host.core_hwm.effective_budget',
  HostCoreHwmApplied: 'zlink.host.core_hwm.applied',
  HostCoreHwmAccounted: 'zlink.host.core_hwm.accounted',
  HostCoreHwmCompletionAccounted: 'zlink.host.core_hwm.completion_accounted',
  HostCoreHwmBlockedRatio: 'zlink.host.core_hwm.blocked_ratio',
  HostApplicationJobQueueLimit: 'zlink.host.application_job_queue.limit',
  HostApplicationJobQueueJobs: 'zlink.host.application_job_queue.jobs',
  HostApplicationJobQueueCapacityWaiters: 'zlink.host.application_job_queue.capacity_waiters',
  HostApplicationJobQueuePressureState: 'zlink.host.application_job_queue.pressure_state',
  HostApplicationJobQueuePauseDuration: 'zlink.host.application_job_queue.pause_duration',
  HostApplicationJobQueueCapacityWaits: 'zlink.host.application_job_queue.capacity_waits',
  HostApplicationJobQueueCapacityWaitDuration:
    'zlink.host.application_job_queue.capacity_wait_duration',
  HostApplicationJobQueuePressureTransitions:
    'zlink.host.application_job_queue.pressure_transitions',
  HostApplicationJobQueueFlowStateConfigFailures:
    'zlink.host.application_job_queue.flow_state_config_failures'
} as const);

const COUNTERS = Object.freeze({
  [METRIC_NAMES.StreamConnectionsOpened]: '{connection}',
  [METRIC_NAMES.StreamConnectionsClosed]: '{connection}',
  [METRIC_NAMES.MeshNodeChannelSelectionFailures]: '{failure}',
  [METRIC_NAMES.MeshNodeRequestTimeouts]: '{request}',
  [METRIC_NAMES.MeshNodeMessagesDropped]: '{message}',
  [METRIC_NAMES.RelocationStarted]: '{relocation}',
  [METRIC_NAMES.RelocationCompleted]: '{relocation}',
  [METRIC_NAMES.RelocationCutoverTimeout]: '{fallback}',
  [METRIC_NAMES.InstanceSpotActivations]: '{activation}',
  [METRIC_NAMES.InstanceSpotClaimConflicts]: '{claim}',
  [METRIC_NAMES.InstanceSpotTakeovers]: '{takeover}',
  [METRIC_NAMES.LocationStoreErrors]: '{error}',
  [METRIC_NAMES.LocationOwnerLeaseRenewFailures]: '{failure}',
  [METRIC_NAMES.ObservabilityEventsOverflow]: '{event}',
  [METRIC_NAMES.HostRelocationBlocked]: '{operation}',
  [METRIC_NAMES.HostShutdownForced]: '{operation}'
} as const);

const UP_DOWN_COUNTERS = Object.freeze({
  [METRIC_NAMES.StreamConnectionsActive]: '{connection}',
  [METRIC_NAMES.SpotCount]: '{spot}',
  [METRIC_NAMES.ActorCount]: '{actor}',
  [METRIC_NAMES.MeshNodeRequestsInflight]: '{request}'
} as const);

const HISTOGRAMS = Object.freeze({
  [METRIC_NAMES.MeshNodeRequestDuration]: 's',
  [METRIC_NAMES.RelocationDuration]: 's',
  [METRIC_NAMES.RelocationBytes]: 'By',
  [METRIC_NAMES.RelocationInterruption]: 's',
  [METRIC_NAMES.RelocationTargetResume]: 's',
  [METRIC_NAMES.RelocationRouteConvergence]: 's',
  [METRIC_NAMES.InstanceSpotActivationDuration]: 's',
  [METRIC_NAMES.LocationOwnerLeaseRenewLateness]: 's',
  [METRIC_NAMES.HostRelocationDuration]: 's',
  [METRIC_NAMES.HostShutdownDuration]: 's'
} as const);

const OBSERVABLE_GAUGES = Object.freeze({
  [METRIC_NAMES.MeshNodePeersConfigured]: '{peer}',
  [METRIC_NAMES.MeshNodePeersConnected]: '{peer}',
  [METRIC_NAMES.MeshNodePeersReady]: '{peer}',
  [METRIC_NAMES.MeshNodeChannelsReadyMembers]: '{member}',
  [METRIC_NAMES.ObjectCapacityActive]: '{object}',
  [METRIC_NAMES.ObjectCapacityReserved]: '{object}',
  [METRIC_NAMES.ObjectCapacityLimit]: '{object}',
  [METRIC_NAMES.SpotTypeCapacityActive]: '{spot}',
  [METRIC_NAMES.SpotTypeCapacityReserved]: '{spot}',
  [METRIC_NAMES.SpotTypeCapacityLimit]: '{spot}',
  [METRIC_NAMES.ObjectActivationActive]: '{activation}',
  [METRIC_NAMES.ObjectActivationLimit]: '{activation}',
  [METRIC_NAMES.InstanceSpotPendingMessages]: '{message}',
  [METRIC_NAMES.InstanceSpotPendingBytes]: 'By',
  [METRIC_NAMES.HostState]: '{runtime}',
  [METRIC_NAMES.HostCoreHwmEffectiveBudget]: 'By',
  [METRIC_NAMES.HostCoreHwmApplied]: 'By',
  [METRIC_NAMES.HostCoreHwmAccounted]: 'By',
  [METRIC_NAMES.HostCoreHwmCompletionAccounted]: 'By',
  [METRIC_NAMES.HostCoreHwmBlockedRatio]: '{ppm}',
  [METRIC_NAMES.HostApplicationJobQueueLimit]: '{job}',
  [METRIC_NAMES.HostApplicationJobQueueJobs]: '{job}',
  [METRIC_NAMES.HostApplicationJobQueueCapacityWaiters]: '{waiter}',
  [METRIC_NAMES.HostApplicationJobQueuePressureState]: '{state}',
  [METRIC_NAMES.HostApplicationJobQueuePauseDuration]: 's'
} as const);

const OBSERVABLE_COUNTERS = Object.freeze({
  [METRIC_NAMES.HostApplicationJobQueueCapacityWaits]: '{wait}',
  [METRIC_NAMES.HostApplicationJobQueueCapacityWaitDuration]: 's',
  [METRIC_NAMES.HostApplicationJobQueuePressureTransitions]: '{transition}',
  [METRIC_NAMES.HostApplicationJobQueueFlowStateConfigFailures]: '{failure}'
} as const);

type CounterName = keyof typeof COUNTERS;
type UpDownName = keyof typeof UP_DOWN_COUNTERS;
type HistogramName = keyof typeof HISTOGRAMS;

class MetricRegistry {
  readonly counters = new Map<string, CounterInstrument>();
  readonly upDown = new Map<string, UpDownInstrument>();
  readonly histograms = new Map<string, HistogramInstrument>();
  readonly meshSnapshots = new Set<() => ReadonlyArray<ZLinkRuntimeMetricMeshSnapshot>>();
  readonly hostStates = new Set<() => string>();
  readonly hostCapacities = new Set<() => ZLinkHostCapacityStatus>();
  readonly applicationJobQueuePressures = new Set<
    () => ZLinkApplicationJobQueuePressureMetricSnapshot
  >();

  constructor(meter: ZLinkObservableMeter) {
    for (const [name, unit] of Object.entries(COUNTERS)) {
      this.counters.set(name, meter.createCounter(name, { unit }));
    }
    for (const [name, unit] of Object.entries(UP_DOWN_COUNTERS)) {
      this.upDown.set(name, meter.createUpDownCounter(name, { unit }));
    }
    for (const [name, unit] of Object.entries(HISTOGRAMS)) {
      this.histograms.set(name, meter.createHistogram(name, { unit }));
    }
    for (const [name, unit] of Object.entries(OBSERVABLE_GAUGES)) {
      const gauge = meter.createObservableGauge?.(name, { unit });
      gauge?.addCallback((result) => this.observe(name, result));
    }
    for (const [name, unit] of Object.entries(OBSERVABLE_COUNTERS)) {
      const counter = meter.createObservableCounter?.(name, { unit });
      counter?.addCallback((result) => this.observe(name, result));
    }
  }

  private observe(name: string, result: ZLinkObservableResult): void {
    if (
      name === METRIC_NAMES.HostApplicationJobQueuePressureState ||
      name === METRIC_NAMES.HostApplicationJobQueuePressureTransitions ||
      name === METRIC_NAMES.HostApplicationJobQueuePauseDuration ||
      name === METRIC_NAMES.HostApplicationJobQueueFlowStateConfigFailures
    ) {
      for (const provider of this.applicationJobQueuePressures) {
        try {
          this.observeApplicationJobQueuePressure(name, provider(), result);
        } catch {
          // Diagnostics must not affect runtime behavior.
        }
      }
      return;
    }
    if (
      name.startsWith('zlink.host.core_hwm.') ||
      name.startsWith('zlink.host.application_job_queue.')
    ) {
      for (const provider of this.hostCapacities) {
        try {
          this.observeHostCapacity(name, provider(), result);
        } catch {
          // Diagnostics must not affect runtime behavior.
        }
      }
      return;
    }
    if (name === METRIC_NAMES.HostState) {
      for (const provider of this.hostStates) {
        try {
          result.observe(1, { state: provider() });
        } catch {
          // Diagnostics must not affect runtime behavior.
        }
      }
      return;
    }

    for (const provider of this.meshSnapshots) {
      let snapshots: ReadonlyArray<ZLinkRuntimeMetricMeshSnapshot>;
      try {
        snapshots = provider();
      } catch {
        continue;
      }
      for (const snapshot of snapshots) this.observeMesh(name, snapshot, result);
    }
  }

  private observeApplicationJobQueuePressure(
    name: string,
    snapshot: ZLinkApplicationJobQueuePressureMetricSnapshot,
    result: ZLinkObservableResult
  ): void {
    if (name === METRIC_NAMES.HostApplicationJobQueuePressureState) {
      result.observe(1, { state: snapshot.pressureState });
    } else if (name === METRIC_NAMES.HostApplicationJobQueuePressureTransitions) {
      result.observe(metricNumber(snapshot.runningTransitionCount), { state: 'running' });
      result.observe(metricNumber(snapshot.pausedTransitionCount), { state: 'paused' });
    } else if (name === METRIC_NAMES.HostApplicationJobQueuePauseDuration) {
      result.observe(nonNegative(snapshot.currentPauseDurationSeconds), { state: 'current' });
      result.observe(nonNegative(snapshot.cumulativePauseDurationSeconds), { state: 'cumulative' });
    } else if (name === METRIC_NAMES.HostApplicationJobQueueFlowStateConfigFailures) {
      result.observe(metricNumber(snapshot.flowStateConfigFailureCount));
    }
  }

  private observeHostCapacity(
    name: string,
    snapshot: ZLinkHostCapacityStatus,
    result: ZLinkObservableResult
  ): void {
    const core = snapshot.coreHwm;
    const jobs = snapshot.applicationJobQueue;
    if (name === METRIC_NAMES.HostCoreHwmEffectiveBudget) {
      result.observe(metricNumber(core.effectiveBudgetBytes));
    } else if (name === METRIC_NAMES.HostCoreHwmApplied) {
      result.observe(metricNumber(core.totalAppliedHwmBytes));
    } else if (name === METRIC_NAMES.HostCoreHwmAccounted) {
      result.observe(metricNumber(core.currentAccountedBytes), { state: 'current' });
      result.observe(metricNumber(core.peakAccountedBytes), { state: 'peak' });
    } else if (name === METRIC_NAMES.HostCoreHwmCompletionAccounted) {
      result.observe(metricNumber(core.completionCurrentAccountedBytes), { state: 'current' });
      result.observe(metricNumber(core.completionPeakAccountedBytes), { state: 'peak' });
    } else if (name === METRIC_NAMES.HostCoreHwmBlockedRatio) {
      result.observe(metricNumber(core.blockedRatioPpm));
    } else if (name === METRIC_NAMES.HostApplicationJobQueueLimit) {
      result.observe(metricNumber(jobs.effectiveMaxQueuedApplicationJobs));
    } else if (name === METRIC_NAMES.HostApplicationJobQueueJobs) {
      result.observe(metricNumber(jobs.reservedSupplyPermits), { state: 'reserved' });
      result.observe(metricNumber(jobs.queuedApplicationJobs), { state: 'queued' });
      result.observe(metricNumber(jobs.permitsInUse), { state: 'in_use' });
      result.observe(metricNumber(jobs.peakPermitsInUse), { state: 'peak' });
    } else if (name === METRIC_NAMES.HostApplicationJobQueueCapacityWaiters) {
      result.observe(metricNumber(jobs.capacityWaiters));
    } else if (name === METRIC_NAMES.HostApplicationJobQueueCapacityWaits) {
      result.observe(metricNumber(jobs.capacityWaitCount));
    } else if (name === METRIC_NAMES.HostApplicationJobQueueCapacityWaitDuration) {
      result.observe(nonNegative(jobs.capacityWaitDurationSeconds));
    }
  }

  private observeMesh(
    name: string,
    snapshot: ZLinkRuntimeMetricMeshSnapshot,
    result: ZLinkObservableResult
  ): void {
    const mesh = { mesh_name: snapshot.meshName };
    const peer = { ...mesh, source: snapshot.source };
    if (name === METRIC_NAMES.MeshNodePeersConfigured)
      result.observe(nonNegative(snapshot.configuredPeers), peer);
    else if (name === METRIC_NAMES.MeshNodePeersConnected)
      result.observe(nonNegative(snapshot.connectedPeers), peer);
    else if (name === METRIC_NAMES.MeshNodePeersReady)
      result.observe(nonNegative(snapshot.readyPeers), peer);
    else if (name === METRIC_NAMES.MeshNodeChannelsReadyMembers) {
      for (const channel of snapshot.channels) {
        result.observe(nonNegative(channel.readyMembers), {
          ...mesh,
          channel_name: channel.channelName
        });
      }
    } else if (name.startsWith('zlink.object.capacity.')) {
      const field = metricSuffix(name);
      result.observe(capacityValue(snapshot.actorCapacity, field), {
        ...mesh,
        capacity_scope: 'actor'
      });
      result.observe(capacityValue(snapshot.spotCapacity, field), {
        ...mesh,
        capacity_scope: 'spot'
      });
    } else if (name.startsWith('zlink.spot.type.capacity.')) {
      const field = metricSuffix(name);
      for (const capacity of snapshot.spotTypeCapacities) {
        result.observe(capacityValue(capacity, field), {
          ...mesh,
          spot_kind: capacity.spotKind,
          stable_type: capacity.stableType
        });
      }
    } else if (name === METRIC_NAMES.ObjectActivationActive) {
      result.observe(nonNegative(snapshot.activation.active), mesh);
    } else if (name === METRIC_NAMES.ObjectActivationLimit) {
      result.observe(nonNegative(snapshot.activation.limit), mesh);
    } else if (name === METRIC_NAMES.InstanceSpotPendingMessages) {
      for (const instance of snapshot.instanceSpots) {
        result.observe(nonNegative(instance.pendingMessages), {
          ...mesh,
          instance_spot_type: instance.instanceSpotType
        });
      }
    } else if (name === METRIC_NAMES.InstanceSpotPendingBytes) {
      for (const instance of snapshot.instanceSpots) {
        result.observe(nonNegative(instance.pendingBytes), {
          ...mesh,
          instance_spot_type: instance.instanceSpotType
        });
      }
    }
  }
}

const registries = new WeakMap<object, MetricRegistry>();

export class ZLinkRuntimeMetrics {
  private readonly registry: MetricRegistry;

  constructor(provider?: ZLinkMeterProvider) {
    const owner = (provider ?? openTelemetryMetrics) as object;
    let registry = registries.get(owner);
    if (registry === undefined) {
      const meter = (provider ?? openTelemetryMetrics).getMeter(
        ZLinkMeters.Framework
      ) as ZLinkObservableMeter;
      registry = new MetricRegistry(meter);
      registries.set(owner, registry);
    }
    this.registry = registry;
  }

  count(name: CounterName | string, value = 1, attributes?: ZLinkMetricAttributes): void {
    safe(() => this.registry.counters.get(name)?.add(value, attributes));
  }

  change(name: UpDownName | string, value: number, attributes?: ZLinkMetricAttributes): void {
    safe(() => this.registry.upDown.get(name)?.add(value, attributes));
  }

  duration(
    name: HistogramName | string,
    seconds: number,
    attributes?: ZLinkMetricAttributes
  ): void {
    this.histogram(name, Math.max(0, seconds), 's', attributes);
  }

  histogram(
    name: HistogramName | string,
    value: number,
    _unit: string,
    attributes?: ZLinkMetricAttributes
  ): void {
    safe(() => this.registry.histograms.get(name)?.record(value, attributes));
  }

  registerMeshSnapshots(
    provider: () => ReadonlyArray<ZLinkRuntimeMetricMeshSnapshot>
  ): ZLinkRuntimeMetricRegistration {
    this.registry.meshSnapshots.add(provider);
    return registration(() => this.registry.meshSnapshots.delete(provider));
  }

  registerHostState(provider: () => string): ZLinkRuntimeMetricRegistration {
    this.registry.hostStates.add(provider);
    return registration(() => this.registry.hostStates.delete(provider));
  }

  registerHostCapacity(provider: () => ZLinkHostCapacityStatus): ZLinkRuntimeMetricRegistration {
    this.registry.hostCapacities.add(provider);
    return registration(() => this.registry.hostCapacities.delete(provider));
  }

  registerApplicationJobQueuePressure(
    provider: () => ZLinkApplicationJobQueuePressureMetricSnapshot
  ): ZLinkRuntimeMetricRegistration {
    this.registry.applicationJobQueuePressures.add(provider);
    return registration(() => this.registry.applicationJobQueuePressures.delete(provider));
  }

  startRequest(
    meshName: string,
    surface: 'node' | 'channel' | 'spot' | 'instance_spot' | 'actor'
  ): ZLinkRequestMetricOperation {
    return new ZLinkRequestMetricOperation(this, meshName, surface);
  }

  recordLocationStoreError(operation: string): void {
    this.count(METRIC_NAMES.LocationStoreErrors, 1, { operation });
  }

  recordOwnerLeaseRenewFailure(scopeKind: 'mesh' | 'channel', scopeName: string): void {
    this.count(METRIC_NAMES.LocationOwnerLeaseRenewFailures, 1, {
      scope_kind: scopeKind,
      scope_name: scopeName
    });
  }

  recordOwnerLeaseRenewLateness(
    seconds: number,
    scopeKind: 'mesh' | 'channel',
    scopeName: string
  ): void {
    this.duration(METRIC_NAMES.LocationOwnerLeaseRenewLateness, seconds, {
      scope_kind: scopeKind,
      scope_name: scopeName
    });
  }

  recordChannelSelectionFailure(meshName: string, channelName: string, reason: string): void {
    this.count(METRIC_NAMES.MeshNodeChannelSelectionFailures, 1, {
      mesh_name: meshName,
      channel_name: channelName,
      reason
    });
  }

  recordMessageDropped(
    meshName: string,
    surface: string,
    messageKind: string,
    reason: string
  ): void {
    this.count(METRIC_NAMES.MeshNodeMessagesDropped, 1, {
      mesh_name: meshName,
      surface,
      message_kind: messageKind,
      reason
    });
  }

  startInstanceSpotActivation(
    meshName: string,
    instanceSpotType: string
  ): ZLinkRuntimeMetricOperation {
    const started = process.hrtime.bigint();
    let completed = false;
    return {
      complete: (outcome: string): void => {
        if (completed) return;
        completed = true;
        const attributes = {
          mesh_name: meshName,
          instance_spot_type: instanceSpotType,
          outcome
        };
        this.count(METRIC_NAMES.InstanceSpotActivations, 1, attributes);
        this.duration(
          METRIC_NAMES.InstanceSpotActivationDuration,
          Number(process.hrtime.bigint() - started) / NANOSECONDS_PER_SECOND,
          attributes
        );
      }
    };
  }

  startRelocation(
    meshName: string,
    objectKind: 'actor' | 'user_spot' | 'instance_spot',
    policy: 'recreate' | 'snapshot'
  ): ZLinkRelocationMetricOperation {
    const started = process.hrtime.bigint();
    const startAttributes = { mesh_name: meshName, object_kind: objectKind, policy };
    let completed = false;
    this.count(METRIC_NAMES.RelocationStarted, 1, startAttributes);
    return {
      recordBytes: (bytes: number): void => {
        if (completed || bytes < 0) return;
        this.histogram(METRIC_NAMES.RelocationBytes, bytes, 'By', startAttributes);
      },
      complete: (outcome: string): void => {
        if (completed) return;
        completed = true;
        const terminal = { ...startAttributes, outcome };
        this.count(METRIC_NAMES.RelocationCompleted, 1, terminal);
        this.duration(
          METRIC_NAMES.RelocationDuration,
          Number(process.hrtime.bigint() - started) / NANOSECONDS_PER_SECOND,
          terminal
        );
      }
    };
  }

  recordInstanceSpotClaimConflict(
    meshName: string,
    instanceSpotType: string,
    reason: string
  ): void {
    this.count(METRIC_NAMES.InstanceSpotClaimConflicts, 1, {
      mesh_name: meshName,
      instance_spot_type: instanceSpotType,
      reason
    });
  }

  recordInstanceSpotTakeover(meshName: string, instanceSpotType: string, outcome: string): void {
    this.count(METRIC_NAMES.InstanceSpotTakeovers, 1, {
      mesh_name: meshName,
      instance_spot_type: instanceSpotType,
      outcome
    });
  }

  enabled(): boolean {
    return true;
  }
}

export class ZLinkRequestMetricOperation {
  private readonly started = process.hrtime.bigint();
  private completed = false;

  constructor(
    private readonly metrics: ZLinkRuntimeMetrics,
    private readonly meshName: string,
    private readonly surface: 'node' | 'channel' | 'spot' | 'instance_spot' | 'actor'
  ) {
    metrics.change(METRIC_NAMES.MeshNodeRequestsInflight, 1, this.baseAttributes());
  }

  complete(outcome: string): void {
    if (this.completed) return;
    this.completed = true;
    const attributes = this.baseAttributes();
    this.metrics.change(METRIC_NAMES.MeshNodeRequestsInflight, -1, attributes);
    this.metrics.duration(
      METRIC_NAMES.MeshNodeRequestDuration,
      Number(process.hrtime.bigint() - this.started) / NANOSECONDS_PER_SECOND,
      { ...attributes, outcome }
    );
    if (outcome === 'timed_out') {
      this.metrics.count(METRIC_NAMES.MeshNodeRequestTimeouts, 1, attributes);
    }
  }

  private baseAttributes(): ZLinkMetricAttributes {
    return { mesh_name: this.meshName, surface: this.surface };
  }
}

function registration(unregister: () => void): ZLinkRuntimeMetricRegistration {
  let active = true;
  return {
    dispose(): void {
      if (!active) return;
      active = false;
      unregister();
    }
  };
}

function metricSuffix(name: string): 'active' | 'reserved' | 'limit' {
  if (name.endsWith('.reserved')) return 'reserved';
  if (name.endsWith('.limit')) return 'limit';
  return 'active';
}

function capacityValue(
  capacity: ZLinkRuntimeMetricCapacity,
  field: 'active' | 'reserved' | 'limit'
): number {
  return nonNegative(capacity[field]);
}

function nonNegative(value: number): number {
  return Math.max(0, value);
}

function metricNumber(value: bigint | number): number {
  return nonNegative(Number(value));
}

function safe(action: () => void): void {
  try {
    action();
  } catch {
    // Metrics listeners are optional and must not affect runtime behavior.
  }
}
