import { ZLinkFrameworkRuntimeState } from '../../contracts';

export type MaintenanceAdmissionState =
  | 'preparing'
  | 'serving'
  | 'retiring'
  | 'draining'
  | 'stopped'
  | 'error';

export type DiscoveryAvailability =
  | 'preparing'
  | 'serving'
  | 'retiring'
  | 'stopped'
  | 'error'
  | 'disconnected';

/** Derives public readiness from the public runtime state authority. */
export function runtimeStateIsReady(state: ZLinkFrameworkRuntimeState): boolean {
  return state === ZLinkFrameworkRuntimeState.Serving;
}

/** Keeps admission gating separate from the public readiness projection. */
export function runtimeAcceptsWork(
  state: ZLinkFrameworkRuntimeState,
  admissionOpen: boolean
): boolean {
  return runtimeStateIsReady(state) && admissionOpen;
}

/** Projects public state into the maintenance admission bounded context. */
export function maintenanceAdmissionState(
  state: ZLinkFrameworkRuntimeState
): MaintenanceAdmissionState {
  switch (state) {
    case ZLinkFrameworkRuntimeState.Preparing: return 'preparing';
    case ZLinkFrameworkRuntimeState.Serving: return 'serving';
    case ZLinkFrameworkRuntimeState.Relocating:
    case ZLinkFrameworkRuntimeState.Relocated: return 'retiring';
    case ZLinkFrameworkRuntimeState.Draining: return 'draining';
    case ZLinkFrameworkRuntimeState.Stopped: return 'stopped';
    case ZLinkFrameworkRuntimeState.Error: return 'error';
  }
}

/** Projects public state into discovery without creating transport-only state. */
export function discoveryAvailabilityForRuntimeState(
  state: ZLinkFrameworkRuntimeState
): Exclude<DiscoveryAvailability, 'disconnected'> {
  switch (state) {
    case ZLinkFrameworkRuntimeState.Preparing: return 'preparing';
    case ZLinkFrameworkRuntimeState.Serving: return 'serving';
    case ZLinkFrameworkRuntimeState.Relocating:
    case ZLinkFrameworkRuntimeState.Relocated:
    case ZLinkFrameworkRuntimeState.Draining: return 'retiring';
    case ZLinkFrameworkRuntimeState.Stopped: return 'stopped';
    case ZLinkFrameworkRuntimeState.Error: return 'error';
  }
}

/** Combines host readiness with the target projection without changing host state. */
export function topologyRuntimeIsReady(
  state: ZLinkFrameworkRuntimeState,
  readyTargetCount: number
): boolean {
  return runtimeStateIsReady(state) && readyTargetCount > 0;
}
