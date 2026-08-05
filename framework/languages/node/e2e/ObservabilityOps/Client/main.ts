import { runObsA1 } from './Scenarios/obs-a1-flow-correlation-scenario.js';
import { runObsA2 } from './Scenarios/obs-a2-error-flow-scenario.js';
import { runObsA3 } from './Scenarios/obs-a3-flow-propagation-scenario.js';
import { runObsA4 } from './Scenarios/obs-a4-fanout-timer-scenario.js';
import { runObsB1 } from './Scenarios/obs-b1-connection-metrics-scenario.js';
import { runObsB2 } from './Scenarios/obs-b2-queue-transfer-metrics-scenario.js';
import { runObsB3 } from './Scenarios/obs-b3-fanout-lease-metrics-scenario.js';
import { runObsB4 } from './Scenarios/obs-b4-disabled-metrics-scenario.js';
import { runObsC1 } from './Scenarios/obs-c1-draining-marker-scenario.js';
import { runObsC2 } from './Scenarios/obs-c2-actor-handoff-scenario.js';
import { runObsC3 } from './Scenarios/obs-c3-spot-drain-policies-scenario.js';
import { runObsC4 } from './Scenarios/obs-c4-forced-session-drain-scenario.js';
import { runObsC5 } from './Scenarios/obs-c5-rollout-scenario.js';
import { closeScenarioClients, options } from './Support/scenario-support.js';

const scenarios: Readonly<Record<string, () => Promise<void>>> = {
  'OBS-A1': runObsA1,
  'OBS-A2': runObsA2,
  'OBS-A3': runObsA3,
  'OBS-A4': runObsA4,
  'OBS-B1': runObsB1,
  'OBS-B2': runObsB2,
  'OBS-B3': runObsB3,
  'OBS-B4': runObsB4,
  'OBS-C1': runObsC1,
  'OBS-C2': runObsC2,
  'OBS-C3': runObsC3,
  'OBS-C4': runObsC4,
  'OBS-C5': runObsC5
};

async function main(): Promise<void> {
  try {
    const selected = options.scenario === 'all'
      ? Object.keys(scenarios)
      : options.scenario.split(',').map((value) => value.trim()).filter(Boolean);
    for (const name of selected) {
      const scenario = scenarios[name];
      if (scenario === undefined) throw new Error(`Unknown scenario '${name}'.`);
      await scenario();
      console.log(`scenario ${name} passed`);
    }
    console.log('observability-ops client result=passed');
  } finally {
    await closeScenarioClients();
  }
}

void runBrowserE2e('ObservabilityOps', main);
import { runBrowserE2e } from '../../browser-client-runtime';
