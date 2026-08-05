import type { ManagedProcess } from './managed-provider';

export interface ScenarioState {
  providerAProcess?: ManagedProcess;
  providerBProcess?: ManagedProcess;
}
