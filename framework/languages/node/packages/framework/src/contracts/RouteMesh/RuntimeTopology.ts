import type { RoutingId } from '../Common';
import type {
  ZLinkApplicationJobQueueProfile,
  ZLinkCoreHwmProfile
} from '../Dispatch';
import type {
  ZLinkPeerState,
  ZLinkTopologyReason,
  ZLinkTopologyState,
  ZLinkObservedStatus
} from './Contracts';
import type { ZLinkFrameworkRuntimeState } from '../Locations';

export enum ZLinkFrameworkRelocationOutcome {
  Relocated = 0,
  Blocked = 1
}

export enum ZLinkFrameworkRelocationMode {
  PlannedMaintenance = 0,
  RollingUpdate = 1
}

export enum ZLinkFrameworkRelocationReason {
  None = 0,
  TargetUnavailable = 1,
  StoreUnavailable = 2,
  RelocationDisabled = 3,
  StateIncompatible = 4,
  DeadlineExceeded = 5,
  RelocationFailed = 6,
  RuntimeNotReady = 7,
  ManualTopologyUnsupported = 8,
  ShutdownRequested = 9,
  OperationInProgress = 10
}

export interface ZLinkFrameworkRelocationOptions {
  readonly mode: ZLinkFrameworkRelocationMode;
  readonly targetApplicationVersion?: bigint;
  readonly deadlineMs?: number;
  readonly signal?: AbortSignal;
}

export interface ZLinkFrameworkRelocationResult {
  readonly mode: ZLinkFrameworkRelocationMode;
  readonly effectiveTargetApplicationVersion: bigint;
  readonly outcome: ZLinkFrameworkRelocationOutcome;
  readonly reason: ZLinkFrameworkRelocationReason;
}

export enum ZLinkFrameworkTerminationOutcome {
  Stopped = 0,
  ForceStopped = 1
}

export enum ZLinkFrameworkTerminationReason {
  None = 0,
  DeadlineExceeded = 1,
  TeardownFailed = 2
}

export interface ZLinkFrameworkTerminationResult {
  readonly outcome: ZLinkFrameworkTerminationOutcome;
  readonly reason: ZLinkFrameworkTerminationReason;
}

export interface ZLinkFrameworkLifecycleOptions {
  readonly deadlineMs?: number;
  readonly signal?: AbortSignal;
}

export interface ZLinkFrameworkRuntimeStatus {
  readonly state: ZLinkFrameworkRuntimeState;
  readonly isReady: boolean;
  readonly acceptingWork: boolean;
  readonly capacity: ZLinkHostCapacityStatus;
  /**
   * True when every relocation this source started reached its Message
   * Follow route removal point and its cutover retransmission window ended
   * (spec 30 §11) — the source-published SafeToShutdown observation.
   */
  readonly safeToShutdown: boolean;
  readonly deadline?: Date;
  readonly relocationResult?: ZLinkFrameworkRelocationResult;
  readonly terminationResult?: ZLinkFrameworkTerminationResult;
  readonly sequence: bigint;
  readonly observedAt: Date;
}

export interface ZLinkCoreHwmStatus {
  readonly configuredMemoryLimitBytes?: bigint;
  readonly configuredBudgetBytes?: bigint;
  readonly configuredProfile: ZLinkCoreHwmProfile;
  readonly effectiveBudgetBytes: bigint;
  readonly totalAppliedHwmBytes: bigint;
  readonly coreQueueAccountedBytes: bigint;
  readonly applicationAccountedBytes: bigint;
  readonly currentAccountedBytes: bigint;
  readonly provisionalAccountedBytes: bigint;
  readonly peakAccountedBytes: bigint;
  readonly completionCurrentAccountedBytes: bigint;
  readonly completionPeakAccountedBytes: bigint;
  readonly completionPendingMessageCount: bigint;
  readonly totalMessagingAccountedBytes: bigint;
  readonly monitorQueueAppliedHwmBytes: bigint;
  readonly monitorQueueAccountedBytes: bigint;
  readonly totalInstanceAppliedHwmBytes: bigint;
  readonly totalInstanceAccountedBytes: bigint;
  readonly blockedRatioPpm: bigint;
  readonly activeDirectionalQueueCount: bigint;
  readonly activeCompletionDirectionalQueueCount: bigint;
  readonly activeSendQueueCount: bigint;
  readonly activeReceiveQueueCount: bigint;
  readonly outstandingApplicationLeaseCount: bigint;
  readonly retiredQueueCount: bigint;
  readonly deferredOriginCreditBytes: bigint;
}

export interface ZLinkApplicationJobQueueStatus {
  readonly configuredProfile: ZLinkApplicationJobQueueProfile;
  readonly configuredManualMax?: bigint;
  readonly effectiveProcessorCount: bigint;
  readonly effectiveMaxQueuedApplicationJobs: bigint;
  readonly reservedSupplyPermits: bigint;
  readonly queuedApplicationJobs: bigint;
  readonly permitsInUse: bigint;
  readonly peakPermitsInUse: bigint;
  readonly capacityWaiters: bigint;
  readonly capacityWaitCount: bigint;
  readonly capacityWaitDurationSeconds: number;
}

export interface ZLinkHostCapacityStatus {
  readonly measurementEpoch: bigint;
  readonly coreHwm: ZLinkCoreHwmStatus;
  readonly applicationJobQueue: ZLinkApplicationJobQueueStatus;
}

export interface ZLinkFrameworkRuntime {
  readonly status: ZLinkFrameworkRuntimeStatus;
  observe(signal?: AbortSignal): AsyncIterable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>>;
  resetCapacityMetrics(): void;
  relocate(options: ZLinkFrameworkRelocationOptions): Promise<ZLinkFrameworkRelocationResult>;
  shutdown(options?: ZLinkFrameworkLifecycleOptions): Promise<ZLinkFrameworkTerminationResult>;
}

export type ZLinkClientServerRole = 'client' | 'server' | 'clientAndServer';

export interface ZLinkClientServerTargetStatus {
  readonly nodeRid: RoutingId;
  readonly weight: number;
  readonly state: ZLinkPeerState;
  readonly unavailableReason?: ZLinkTopologyReason;
}

export interface ZLinkClientServerStatus {
  readonly channelName: string;
  readonly localRole: ZLinkClientServerRole;
  readonly state: ZLinkTopologyState;
  readonly isReady: boolean;
  readonly readyTargetCount: number;
  readonly targets: readonly ZLinkClientServerTargetStatus[];
  readonly sequence: bigint;
  readonly observedAt: Date;
}

export interface ZLinkClientServerRuntime {
  snapshot(channelName: string): ZLinkClientServerStatus;
  observe(
    channelName: string,
    capacity?: number,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkClientServerStatus>>;
  isReady(channelName: string): boolean;
}

export interface ZLinkFanoutStatus {
  readonly channelName: string;
  readonly state: ZLinkTopologyState;
  readonly isReady: boolean;
  readonly readyPublisherCount: number;
  readonly publishers: readonly import('./Contracts').ZLinkPeerStatus[];
  readonly sequence: bigint;
  readonly observedAt: Date;
}

export interface ZLinkFanoutRuntime {
  snapshot(channelName: string): ZLinkFanoutStatus;
  observe(
    channelName: string,
    capacity?: number,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkFanoutStatus>>;
}
