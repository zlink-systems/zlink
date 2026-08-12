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
import { runRlC3 } from './Scenarios/rl-c3-node-pause-recovery-scenario';
import { runRlC4 } from './Scenarios/rl-c4-store-outage-scenario';
import { runRlD2 } from './Scenarios/rl-d2-observer-fault-scenario';
import { runRlD3 } from './Scenarios/rl-d3-dispatch-error-evidence-scenario';
import { runRlD4 } from './Scenarios/rl-d4-missing-request-handler-scenario';
import { runRlD5 } from './Scenarios/rl-d5-mixed-burst-scenario';
import { parseClientOptions } from './Support/client-options';
import type { ScenarioState } from './Support/scenario-state';
import { runRLE1 } from './Scenarios/rl-e1-scenario';
import { runRLE2 } from './Scenarios/rl-e2-scenario';
import { runRLE3 } from './Scenarios/rl-e3-scenario';
import { runRLE4 } from './Scenarios/rl-e4-scenario';
import { runRLE5 } from './Scenarios/rl-e5-scenario';
import { runRLF1 } from './Scenarios/rl-f1-scenario';
import { runRLF3 } from './Scenarios/rl-f3-scenario';
import { runRLF5 } from './Scenarios/rl-f5-scenario';
import { runRLF6 } from './Scenarios/rl-f6-scenario';
import { runRLF7 } from './Scenarios/rl-f7-scenario';
import { runRLF8 } from './Scenarios/rl-f8-scenario';
import { runRLF9 } from './Scenarios/rl-f9-scenario';
import { runRLF10 } from './Scenarios/rl-f10-scenario';
import { runRLF11 } from './Scenarios/rl-f11-scenario';
import { runRLF12 } from './Scenarios/rl-f12-scenario';
import { runRLF13 } from './Scenarios/rl-f13-scenario';
import { runRLF14 } from './Scenarios/rl-f14-scenario';

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
    'RL-C3': () => runRlC3(options, state),
    'RL-C4': () => runRlC4(options, state),
    'RL-D2': () => runRlD2(options),
    'RL-D3': () => runRlD3(options),
    'RL-D4': () => runRlD4(options),
    'RL-D5': () => runRlD5(options),
    'RL-E1': () => runRLE1(options),
    'RL-E2': () => runRLE2(options),
    'RL-E3': () => runRLE3(options),
    'RL-E4': () => runRLE4(options),
    'RL-E5': () => runRLE5(options),
    'RL-F1': () => runRLF1(options),
    'RL-F3': () => runRLF3(options),
    'RL-F5': () => runRLF5(options),
    'RL-F6': () => runRLF6(options),
    'RL-F7': () => runRLF7(options),
    'RL-F8': () => runRLF8(options),
    'RL-F9': () => runRLF9(options),
    'RL-F10': () => runRLF10(options),
    'RL-F11': () => runRLF11(options),
    'RL-F12': () => runRLF12(options),
    'RL-F13': () => runRLF13(options),
    'RL-F14': () => runRLF14(options),
  };
  const gaps: Record<string, string> = {};
  const defaultScenarioIds = ['RL-A1', 'RL-A2', 'RL-A3', 'RL-A4', 'RL-A5', 'RL-B1', 'RL-B2', 'RL-B3', 'RL-B4', 'RL-B5', 'RL-B6', 'RL-C1', 'RL-C3', 'RL-C4', 'RL-D2', 'RL-D3', 'RL-D4', 'RL-D5', 'RL-E1', 'RL-E2', 'RL-E3', 'RL-E4', 'RL-E5', 'RL-F1', 'RL-F3', 'RL-F5', 'RL-F6', 'RL-F7', 'RL-F8', 'RL-F9', 'RL-F10', 'RL-F11', 'RL-F12', 'RL-F13', 'RL-F14'];

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
