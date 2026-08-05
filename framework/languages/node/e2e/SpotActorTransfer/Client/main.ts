import { runBrowserE2e } from '../../browser-client-runtime';
import { runStA1 } from './Scenarios/st-a1-local-join-scenario';
import { runStA2 } from './Scenarios/st-a2-reject-rollback-scenario';
import { runStA3 } from './Scenarios/st-a3-concurrent-join-scenario';
import { runStB1 } from './Scenarios/st-b1-remote-transfer-scenario';
import { runStB2 } from './Scenarios/st-b2-source-cleanup-loss-scenario';
import { runStB3 } from './Scenarios/st-b3-source-failure-scenario';
import { runStB4 } from './Scenarios/st-b4-target-failure-rollback-scenario';
import { runStC1 } from './Scenarios/st-c1-duplicate-transfer-scenario';
import { runStC2 } from './Scenarios/st-c2-stale-generation-scenario';
import { runStC3 } from './Scenarios/st-c3-callback-failure-scenario';
import { runStD1 } from './Scenarios/st-d1-stateless-transfer-scenario';
import { runStD2 } from './Scenarios/st-d2-stateful-transfer-scenario';
import { runStE1 } from './Scenarios/st-e1-source-restart-recovery-scenario';
import { runStE1A } from './Scenarios/st-e1a-new-incarnation-explicit-bind-scenario';
import { runStE2 } from './Scenarios/st-e2-target-restart-recovery-scenario';
import { runStF1 } from './Scenarios/st-f1-packet-order-scenario';
import { runStF2 } from './Scenarios/st-f2-in-flight-order-scenario';
import { runStF3 } from './Scenarios/st-f3-request-order-scenario';
import { runStF4 } from './Scenarios/st-f4-bound-session-transfer-scenario';
import { runStF5 } from './Scenarios/st-f5-external-route-scenario';
import { runStF6 } from './Scenarios/st-f6-backpressure-timeout-scenario';
import {
  prepareStH2TargetRestart,
  verifyStH2TargetRestart
} from './Scenarios/st-h2-target-process-restart-scenario';
import { runStI4 } from './Scenarios/st-i4-message-follow-matrix-scenario';
import { runStI5 } from './Scenarios/st-i5-message-follow-safety-scenario';
import { runStI6 } from './Scenarios/st-i6-multi-hop-message-follow-scenario';
import { runStI1 } from './Scenarios/st-i1-payload-gate-scenario';
import { runStI2 } from './Scenarios/st-i2-bulk-actor-relocation-scenario';
import { runStI3 } from './Scenarios/st-i3-bulk-spot-relocation-scenario';
import { runStH1 } from './Scenarios/st-h1-deferred-join-barrier-scenario';
import { runStH3 } from './Scenarios/st-h3-context-fence-scenario';
import { runStH4 } from './Scenarios/st-h4-deferred-join-rejection-scenario';
import { runStH5 } from './Scenarios/st-h5-message-context-scenario';
import { closeScenarioClients, options } from './Support/scenario-support';

const scenarios: Readonly<Record<string, () => Promise<void>>> = {
  'ST-A1': runStA1, 'ST-A2': runStA2, 'ST-A3': runStA3,
  'ST-B1': runStB1, 'ST-B2': runStB2, 'ST-B3': runStB3, 'ST-B4': runStB4,
  'ST-C1': runStC1, 'ST-C2': runStC2, 'ST-C3': runStC3,
  'ST-D1': runStD1, 'ST-D2': runStD2,
  'ST-E1': runStE1, 'ST-E2': runStE2,
  'ST-E1A': runStE1A,
  'ST-F1': runStF1, 'ST-F2': runStF2, 'ST-F3': runStF3,
  'ST-F4': runStF4, 'ST-F5': runStF5,
  'ST-F6': runStF6,
  'ST-H2-PREPARE': prepareStH2TargetRestart,
  'ST-H2-VERIFY': verifyStH2TargetRestart,
  'ST-H1': runStH1,
  'ST-H3': runStH3,
  'ST-H4': runStH4,
  'ST-H5': runStH5,
  'ST-I1': runStI1,
  'ST-I2': runStI2,
  'ST-I3': runStI3,
  'ST-I4': runStI4,
  'ST-I5': runStI5,
  'ST-I6': runStI6
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
    console.log('spot-actor-transfer e2e result=passed');
  } finally {
    await closeScenarioClients();
  }
}

void runBrowserE2e('SpotActorTransfer', main);
