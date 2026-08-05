import { runRlA1 } from './Scenarios/rl-a1-provider-restart-scenario';
import { runRlA2 } from './Scenarios/rl-a2-provider-endpoint-remap-scenario';
import { runRlA3 } from './Scenarios/rl-a3-reconnect-storm-scenario';
import { runRlA4 } from './Scenarios/rl-a4-drain-and-green-endpoint-scenario';
import { runRlA5 } from './Scenarios/rl-a5-provider-flapping-scenario';
import { runRlB1 } from './Scenarios/rl-b1-cancellation-cleanup-scenario';
import { runRlB2 } from './Scenarios/rl-b2-crash-during-inflight-scenario';
import { runRlB3 } from './Scenarios/rl-b3-graceful-shutdown-scenario';
import { runRlB4 } from './Scenarios/rl-b4-runtime-drain-scenario';
import { runRlB5 } from './Scenarios/rl-b5-drain-inflight-scenario';
import { runRlB6 } from './Scenarios/rl-b6-gray-fault-scenario';
import { runRlC1 } from './Scenarios/rl-c1-client-host-lifecycle-scenario';
import { runRlC2 } from './Scenarios/rl-c2-topology-recovery-scenario';
import { runRlC3 } from './Scenarios/rl-c3-node-pause-recovery-scenario';
import { runRlC4 } from './Scenarios/rl-c4-store-outage-scenario';
import { runRlD1 } from './Scenarios/rl-d1-high-fanout-scenario';
import { runRlD2 } from './Scenarios/rl-d2-observer-fault-scenario';
import { runRlD3 } from './Scenarios/rl-d3-dispatch-error-evidence-scenario';
import { runRlD4 } from './Scenarios/rl-d4-missing-request-handler-scenario';
import { runRlD5 } from './Scenarios/rl-d5-mixed-burst-scenario';
import { parseClientOptions } from './Support/client-options';
import type { ScenarioState } from './Support/scenario-state';

async function main(): Promise<void> {
  const options = parseClientOptions(process.argv.slice(2));
  const state: ScenarioState = {};
  const scenarios: Record<string, () => Promise<void>> = {
    'RL-A1': () => runRlA1(options, state),
    'RL-A2': () => runRlA2(options, state),
    'RL-A3': () => runRlA3(options),
    'RL-A4': () => runRlA4(options, state),
    'RL-A5': () => runRlA5(options, state),
    'RL-B1': () => runRlB1(options),
    'RL-B2': () => runRlB2(options, state),
    'RL-B3': () => runRlB3(options, state),
    'RL-B4': () => runRlB4(options),
    'RL-B5': () => runRlB5(options),
    'RL-B6': () => runRlB6(options),
    'RL-C1': () => runRlC1(options),
    'RL-C2': () => runRlC2(options, state),
    'RL-C3': () => runRlC3(options, state),
    'RL-C4': () => runRlC4(options, state),
    'RL-D1': () => runRlD1(options),
    'RL-D2': () => runRlD2(options),
    'RL-D3': () => runRlD3(options),
    'RL-D4': () => runRlD4(options),
    'RL-D5': () => runRlD5(options)
  };
  const gaps: Record<string, string> = {};
  const defaultScenarioIds = ['RL-A1', 'RL-A2', 'RL-A3', 'RL-A4', 'RL-A5', 'RL-B1', 'RL-B2', 'RL-B3', 'RL-B4', 'RL-B5', 'RL-B6', 'RL-C1', 'RL-C2', 'RL-C3', 'RL-C4', 'RL-D1', 'RL-D2', 'RL-D3', 'RL-D4', 'RL-D5'];

  try {
    if (options.scenario.toLowerCase() === 'all') {
      for (const scenarioId of defaultScenarioIds) {
        await scenarios[scenarioId]();
      }
    } else {
      const selected = options.scenario.toUpperCase();
      const gap = gaps[selected];
      if (gap !== undefined) {
        throw new Error(`Scenario ${selected} is a public contract gap: ${gap}`);
      }
      const scenario = scenarios[selected];
      if (scenario === undefined) {
        throw new Error(`Unknown scenario '${options.scenario}'.`);
      }
      await scenario();
    }
  } finally {
    await state.providerAProcess?.stop();
    state.providerAProcess = undefined;
    await state.providerBProcess?.stop();
    state.providerBProcess = undefined;
  }

  console.log('resilience-lifecycle e2e result=passed');
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
