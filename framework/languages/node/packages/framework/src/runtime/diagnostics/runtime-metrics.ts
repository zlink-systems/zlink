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
  createObservableGauge?(
    name: string,
    options?: { readonly unit?: string }
  ): ZLinkObservableGauge;
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

const COUNTERS = Object.freeze({
  'zlink.stream.connections.opened': '{connection}',
  'zlink.stream.connections.closed': '{connection}',
  'zlink.mesh_node.channel.selection_failures': '{failure}',
  'zlink.mesh_node.request.timeouts': '{request}',
  'zlink.mesh_node.messages.dropped': '{message}',
  'zlink.relocation.started': '{relocation}',
  'zlink.relocation.completed': '{relocation}',
  'zlink.relocation.cutover_timeout': '{fallback}',
  'zlink.instance_spot.activations': '{activation}',
  'zlink.instance_spot.claim.conflicts': '{claim}',
  'zlink.instance_spot.takeovers': '{takeover}',
  'zlink.location.store.errors': '{error}',
  'zlink.location.owner_lease.renew.failures': '{failure}',
  'zlink.observability.events.overflow': '{event}',
  'zlink.host.relocation.blocked': '{operation}',
  'zlink.host.shutdown.forced': '{operation}'
} as const);

const UP_DOWN_COUNTERS = Object.freeze({
  'zlink.stream.connections.active': '{connection}',
  'zlink.spot.count': '{spot}',
  'zlink.actor.count': '{actor}',
  'zlink.mesh_node.requests.inflight': '{request}'
} as const);

const HISTOGRAMS = Object.freeze({
  'zlink.mesh_node.request.duration': 's',
  'zlink.relocation.duration': 's',
  'zlink.relocation.bytes': 'By',
  'zlink.relocation.interruption': 's',
  'zlink.relocation.target_resume': 's',
  'zlink.relocation.route_convergence': 's',
  'zlink.instance_spot.activation.duration': 's',
  'zlink.location.owner_lease.renew.lateness': 's',
  'zlink.host.relocation.duration': 's',
  'zlink.host.shutdown.duration': 's'
} as const);

const OBSERVABLE_GAUGES = Object.freeze({
  'zlink.mesh_node.peers.configured': '{peer}',
  'zlink.mesh_node.peers.connected': '{peer}',
  'zlink.mesh_node.peers.ready': '{peer}',
  'zlink.mesh_node.channels.ready_members': '{member}',
  'zlink.object.capacity.active': '{object}',
  'zlink.object.capacity.reserved': '{object}',
  'zlink.object.capacity.limit': '{object}',
  'zlink.spot.type.capacity.active': '{spot}',
  'zlink.spot.type.capacity.reserved': '{spot}',
  'zlink.spot.type.capacity.limit': '{spot}',
  'zlink.object.activation.active': '{activation}',
  'zlink.object.activation.limit': '{activation}',
  'zlink.instance_spot.pending.messages': '{message}',
  'zlink.instance_spot.pending.bytes': 'By',
  'zlink.host.state': '{runtime}',
  'zlink.host.core_hwm.effective_budget': 'By',
  'zlink.host.core_hwm.applied': 'By',
  'zlink.host.core_hwm.accounted': 'By',
  'zlink.host.core_hwm.completion_accounted': 'By',
  'zlink.host.core_hwm.blocked_ratio': '{ppm}',
  'zlink.host.application_job_queue.limit': '{job}',
  'zlink.host.application_job_queue.jobs': '{job}',
  'zlink.host.application_job_queue.capacity_waiters': '{waiter}',
  'zlink.host.application_job_queue.pressure_state': '{state}',
  'zlink.host.application_job_queue.pause_duration': 's'
} as const);

const OBSERVABLE_COUNTERS = Object.freeze({
  'zlink.host.application_job_queue.capacity_waits': '{wait}',
  'zlink.host.application_job_queue.capacity_wait_duration': 's',
  'zlink.host.application_job_queue.pressure_transitions': '{transition}',
  'zlink.host.application_job_queue.flow_state_config_failures': '{failure}'
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
    if (name === 'zlink.host.application_job_queue.pressure_state'
        || name === 'zlink.host.application_job_queue.pressure_transitions'
        || name === 'zlink.host.application_job_queue.pause_duration'
        || name === 'zlink.host.application_job_queue.flow_state_config_failures') {
      for (const provider of this.applicationJobQueuePressures) {
        try {
          this.observeApplicationJobQueuePressure(name, provider(), result);
        } catch {
          // Diagnostics must not affect runtime behavior.
        }
      }
      return;
    }
    if (name.startsWith('zlink.host.core_hwm.')
        || name.startsWith('zlink.host.application_job_queue.')) {
      for (const provider of this.hostCapacities) {
        try {
          this.observeHostCapacity(name, provider(), result);
        } catch {
          // Diagnostics must not affect runtime behavior.
        }
      }
      return;
    }
    if (name === 'zlink.host.state') {
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
    if (name === 'zlink.host.application_job_queue.pressure_state') {
      result.observe(1, { state: snapshot.pressureState });
    } else if (name === 'zlink.host.application_job_queue.pressure_transitions') {
      result.observe(metricNumber(snapshot.runningTransitionCount), { state: 'running' });
      result.observe(metricNumber(snapshot.pausedTransitionCount), { state: 'paused' });
    } else if (name === 'zlink.host.application_job_queue.pause_duration') {
      result.observe(nonNegative(snapshot.currentPauseDurationSeconds), { state: 'current' });
      result.observe(nonNegative(snapshot.cumulativePauseDurationSeconds), { state: 'cumulative' });
    } else if (name === 'zlink.host.application_job_queue.flow_state_config_failures') {
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
    if (name === 'zlink.host.core_hwm.effective_budget') {
      result.observe(metricNumber(core.effectiveBudgetBytes));
    } else if (name === 'zlink.host.core_hwm.applied') {
      result.observe(metricNumber(core.totalAppliedHwmBytes));
    } else if (name === 'zlink.host.core_hwm.accounted') {
      result.observe(metricNumber(core.currentAccountedBytes), { state: 'current' });
      result.observe(metricNumber(core.peakAccountedBytes), { state: 'peak' });
    } else if (name === 'zlink.host.core_hwm.completion_accounted') {
      result.observe(metricNumber(core.completionCurrentAccountedBytes), { state: 'current' });
      result.observe(metricNumber(core.completionPeakAccountedBytes), { state: 'peak' });
    } else if (name === 'zlink.host.core_hwm.blocked_ratio') {
      result.observe(metricNumber(core.blockedRatioPpm));
    } else if (name === 'zlink.host.application_job_queue.limit') {
      result.observe(metricNumber(jobs.effectiveMaxQueuedApplicationJobs));
    } else if (name === 'zlink.host.application_job_queue.jobs') {
      result.observe(metricNumber(jobs.reservedSupplyPermits), { state: 'reserved' });
      result.observe(metricNumber(jobs.queuedApplicationJobs), { state: 'queued' });
      result.observe(metricNumber(jobs.permitsInUse), { state: 'in_use' });
      result.observe(metricNumber(jobs.peakPermitsInUse), { state: 'peak' });
    } else if (name === 'zlink.host.application_job_queue.capacity_waiters') {
      result.observe(metricNumber(jobs.capacityWaiters));
    } else if (name === 'zlink.host.application_job_queue.capacity_waits') {
      result.observe(metricNumber(jobs.capacityWaitCount));
    } else if (name === 'zlink.host.application_job_queue.capacity_wait_duration') {
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
    if (name === 'zlink.mesh_node.peers.configured') result.observe(nonNegative(snapshot.configuredPeers), peer);
    else if (name === 'zlink.mesh_node.peers.connected') result.observe(nonNegative(snapshot.connectedPeers), peer);
    else if (name === 'zlink.mesh_node.peers.ready') result.observe(nonNegative(snapshot.readyPeers), peer);
    else if (name === 'zlink.mesh_node.channels.ready_members') {
      for (const channel of snapshot.channels) {
        result.observe(nonNegative(channel.readyMembers), { ...mesh, channel_name: channel.channelName });
      }
    } else if (name.startsWith('zlink.object.capacity.')) {
      const field = metricSuffix(name);
      result.observe(capacityValue(snapshot.actorCapacity, field), { ...mesh, capacity_scope: 'actor' });
      result.observe(capacityValue(snapshot.spotCapacity, field), { ...mesh, capacity_scope: 'spot' });
    } else if (name.startsWith('zlink.spot.type.capacity.')) {
      const field = metricSuffix(name);
      for (const capacity of snapshot.spotTypeCapacities) {
        result.observe(capacityValue(capacity, field), {
          ...mesh,
          spot_kind: capacity.spotKind,
          stable_type: capacity.stableType
        });
      }
    } else if (name === 'zlink.object.activation.active') {
      result.observe(nonNegative(snapshot.activation.active), mesh);
    } else if (name === 'zlink.object.activation.limit') {
      result.observe(nonNegative(snapshot.activation.limit), mesh);
    } else if (name === 'zlink.instance_spot.pending.messages') {
      for (const instance of snapshot.instanceSpots) {
        result.observe(nonNegative(instance.pendingMessages), {
          ...mesh,
          instance_spot_type: instance.instanceSpotType
        });
      }
    } else if (name === 'zlink.instance_spot.pending.bytes') {
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
      const meter = (provider ?? openTelemetryMetrics).getMeter(ZLinkMeters.Framework) as ZLinkObservableMeter;
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

  duration(name: HistogramName | string, seconds: number, attributes?: ZLinkMetricAttributes): void {
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

  registerHostCapacity(
    provider: () => ZLinkHostCapacityStatus
  ): ZLinkRuntimeMetricRegistration {
    this.registry.hostCapacities.add(provider);
    return registration(() => this.registry.hostCapacities.delete(provider));
  }

  registerApplicationJobQueuePressure(
    provider: () => ZLinkApplicationJobQueuePressureMetricSnapshot
  ): ZLinkRuntimeMetricRegistration {
    this.registry.applicationJobQueuePressures.add(provider);
    return registration(() => this.registry.applicationJobQueuePressures.delete(provider));
  }

  startRequest(meshName: string, surface: 'node' | 'channel' | 'spot' | 'instance_spot' | 'actor'):
    ZLinkRequestMetricOperation {
    return new ZLinkRequestMetricOperation(this, meshName, surface);
  }

  recordLocationStoreError(operation: string): void {
    this.count('zlink.location.store.errors', 1, { operation });
  }

  recordOwnerLeaseRenewFailure(scopeKind: 'mesh' | 'channel', scopeName: string): void {
    this.count('zlink.location.owner_lease.renew.failures', 1, {
      scope_kind: scopeKind,
      scope_name: scopeName
    });
  }

  recordOwnerLeaseRenewLateness(
    seconds: number,
    scopeKind: 'mesh' | 'channel',
    scopeName: string
  ): void {
    this.duration('zlink.location.owner_lease.renew.lateness', seconds, {
      scope_kind: scopeKind,
      scope_name: scopeName
    });
  }

  recordChannelSelectionFailure(meshName: string, channelName: string, reason: string): void {
    this.count('zlink.mesh_node.channel.selection_failures', 1, {
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
    this.count('zlink.mesh_node.messages.dropped', 1, {
      mesh_name: meshName,
      surface,
      message_kind: messageKind,
      reason
    });
  }

  startInstanceSpotActivation(meshName: string, instanceSpotType: string): ZLinkRuntimeMetricOperation {
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
        this.count('zlink.instance_spot.activations', 1, attributes);
        this.duration(
          'zlink.instance_spot.activation.duration',
          Number(process.hrtime.bigint() - started) / 1e9,
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
    this.count('zlink.relocation.started', 1, startAttributes);
    return {
      recordBytes: (bytes: number): void => {
        if (completed || bytes < 0) return;
        this.histogram('zlink.relocation.bytes', bytes, 'By', startAttributes);
      },
      complete: (outcome: string): void => {
        if (completed) return;
        completed = true;
        const terminal = { ...startAttributes, outcome };
        this.count('zlink.relocation.completed', 1, terminal);
        this.duration(
          'zlink.relocation.duration',
          Number(process.hrtime.bigint() - started) / 1e9,
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
    this.count('zlink.instance_spot.claim.conflicts', 1, {
      mesh_name: meshName,
      instance_spot_type: instanceSpotType,
      reason
    });
  }

  recordInstanceSpotTakeover(meshName: string, instanceSpotType: string, outcome: string): void {
    this.count('zlink.instance_spot.takeovers', 1, {
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
    metrics.change('zlink.mesh_node.requests.inflight', 1, this.baseAttributes());
  }

  complete(outcome: string): void {
    if (this.completed) return;
    this.completed = true;
    const attributes = this.baseAttributes();
    this.metrics.change('zlink.mesh_node.requests.inflight', -1, attributes);
    this.metrics.duration(
      'zlink.mesh_node.request.duration',
      Number(process.hrtime.bigint() - this.started) / 1e9,
      { ...attributes, outcome }
    );
    if (outcome === 'timed_out') {
      this.metrics.count('zlink.mesh_node.request.timeouts', 1, attributes);
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
