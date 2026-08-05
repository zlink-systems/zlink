import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import { runTdA1 } from './Scenarios/td-a1-terminator-surface-scenario';
import { runTdA2 } from './Scenarios/td-a2-async-completion-order-scenario';
import { runTdA3 } from './Scenarios/td-a3-async-counter-serialization-scenario';
import { runTdA4 } from './Scenarios/td-a4-delayed-async-completion-scenario';
import { runTdA5 } from './Scenarios/td-a5-async-timer-exclusion-scenario';
import { runTdB1 } from './Scenarios/td-b1-yield-probe-interleave-scenario';
import { runTdB2 } from './Scenarios/td-b2-yield-queued-probe-order-scenario';
import { runTdB3 } from './Scenarios/td-b3-yield-lost-update-scenario';
import { runTdB4 } from './Scenarios/td-b4-yield-timer-interleave-scenario';
import { runTdC1 } from './Scenarios/td-c1-http-yield-interleave-scenario';
import { runTdC2 } from './Scenarios/td-c2-http-async-exclusion-scenario';
import { runTdC3 } from './Scenarios/td-c3-io-worker-capacity-scenario';
import { runTdC4 } from './Scenarios/td-c4-cpu-worker-turn-order-scenario';
import { runTdC5 } from './Scenarios/td-c5-cpu-worker-source-gate-scenario';
import { runTdD1 } from './Scenarios/td-d1-cross-actor-yield-interleave-scenario';
import { runTdD2 } from './Scenarios/td-d2-same-actor-no-reentry-scenario';
import { runTdD3 } from './Scenarios/td-d3-timer-no-reentry-scenario';
import { runTdD4 } from './Scenarios/td-d4-per-actor-lane-scenario';
import { runTdD5 } from './Scenarios/td-d5-unsupported-yield-scenario';
import { runTdD6 } from './Scenarios/td-d6-same-gate-rejection-scenario';
import { runTdE1 } from './Scenarios/td-e1-entry-to-user-spot-join-scenario';
import { runTdE2 } from './Scenarios/td-e2-user-to-user-spot-join-scenario';
import { runTdE3 } from './Scenarios/td-e3-opposite-spot-join-scenario';
import { runTdE2A } from './Scenarios/td-e2a-deferred-join-failure-scenario';
import { runTdF1 } from './Scenarios/td-f1-remote-spot-continuation-scenario';
import { runTdF2 } from './Scenarios/td-f2-route-bridge-yield-scenario';
import { runTdF3 } from './Scenarios/td-f3-session-relay-yield-scenario';
import { runTdF4 } from './Scenarios/td-f4-request-timeout-recovery-scenario';
import { runTdF5 } from './Scenarios/td-f5-cancellation-shutdown-recovery-scenario';
import { runTdF5A } from './Scenarios/td-f5a-host-shutdown-scenario';
import { runTdF6 } from './Scenarios/td-f6-self-request-timeout-recovery-scenario';
import { runTdG1 } from './Scenarios/td-g1-terminator-conformance-scenario';
import { ExecutionTurnScenarioSuite } from './Support/execution-turn-scenario-suite';
import { runShutdownAdmission, runShutdownRecovery, runShutdownWait } from './Support/shutdown-probe';
import { parseClientOptions } from './Support/client-options';
import { browserE2eConfig, runBrowserE2e } from '../../browser-client-runtime';

const scenarios = new Map<string, (suite: ExecutionTurnScenarioSuite) => Promise<void>>([
  ['TD-A1', runTdA1], ['TD-A2', runTdA2], ['TD-A3', runTdA3], ['TD-A4', runTdA4], ['TD-A5', runTdA5],
  ['TD-B1', runTdB1], ['TD-B2', runTdB2], ['TD-B3', runTdB3], ['TD-B4', runTdB4],
  ['TD-C1', runTdC1], ['TD-C2', runTdC2], ['TD-C3', runTdC3], ['TD-C4', runTdC4], ['TD-C5', runTdC5],
  ['TD-D1', runTdD1], ['TD-D2', runTdD2], ['TD-D3', runTdD3], ['TD-D4', runTdD4], ['TD-D5', runTdD5], ['TD-D6', runTdD6],
  ['TD-E1', runTdE1], ['TD-E2', runTdE2], ['TD-E3', runTdE3], ['TD-E2A', runTdE2A],
  ['TD-F1', runTdF1], ['TD-F2', runTdF2], ['TD-F3', runTdF3], ['TD-F4', runTdF4], ['TD-F5', runTdF5],
  ['TD-F5A', runTdF5A], ['TD-F6', runTdF6], ['TD-G1', runTdG1]
]);

async function main(): Promise<void> {
  const options = parseClientOptions(await browserE2eConfig());
  const scenario = options.scenario.toUpperCase();
  const full = scenario === 'FULL' || scenario === 'ALL';
  if (scenario === 'SHUTDOWN-WAIT') {
    await runShutdownWait(options);
    return;
  }
  if (scenario === 'SHUTDOWN-RECOVERY') {
    await runShutdownRecovery(options);
    return;
  }
  if (scenario === 'SHUTDOWN-ADMISSION') {
    await runShutdownAdmission(options);
    return;
  }

  const client = createClient(options.sessionAStreamEndpoint);
  await client.connect();
  try {
    const suite = new ExecutionTurnScenarioSuite(client);
    if (full) {
      for (const [id, run] of scenarios) {
        await run(suite);
        console.log(`${id} result=passed`);
      }
    } else {
      const run = scenarios.get(scenario);
      if (run === undefined) throw new Error(`Unknown scenario '${options.scenario}'.`);
      await run(suite);
      console.log(`${scenario} result=passed`);
    }
  } finally {
    await client.close();
  }

  console.log('execution-turn client result=passed');
}

function createClient(endpoint: string) {
  return zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 30000,
    requestTimeoutMs: 60000,
    maxReceivedMessages: 1024
  });
}

void runBrowserE2e('AutomaticTurnDispatch', main);
