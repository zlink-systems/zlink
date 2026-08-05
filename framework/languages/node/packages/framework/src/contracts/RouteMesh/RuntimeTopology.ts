import type { RoutingId } from '../Common';
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
  readonly deadline?: Date;
  readonly relocationResult?: ZLinkFrameworkRelocationResult;
  readonly terminationResult?: ZLinkFrameworkTerminationResult;
  readonly inboundDispatch: ZLinkInboundDispatchStatus;
  readonly sequence: bigint;
  readonly observedAt: Date;
}

export interface ZLinkInboundDispatchStatus {
  readonly applicationHwmBytes: bigint;
  readonly pendingPayloadBytes: bigint;
  readonly queuedPayloadBytes: bigint;
  readonly activePayloadBytes: bigint;
  readonly applicationReceivePaused: boolean;
  readonly pendingCompletionSends: bigint;
  readonly completionSendLimit: bigint;
}

export interface ZLinkFrameworkRuntime {
  readonly status: ZLinkFrameworkRuntimeStatus;
  observe(signal?: AbortSignal): AsyncIterable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>>;
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
