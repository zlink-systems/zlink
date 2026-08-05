import { parseClientOptions } from './Support/client-options';
import { runCh01 } from './Scenarios/ch-e2e-01-route-request-scenario';
import { runCh02 } from './Scenarios/ch-e2e-02-nested-request-scenario';
import { runCh03 } from './Scenarios/ch-e2e-03-spot-timer-scenario';
import { runCh04A } from './Scenarios/ch-e2e-04a-weight-scenario';
import { runCh04B } from './Scenarios/ch-e2e-04b-drain-scenario';
import { runCh04C } from './Scenarios/ch-e2e-04c-restart-scenario';
import { runCh05 } from './Scenarios/ch-e2e-05-client-role-scenario';
import { runCh06 } from './Scenarios/ch-e2e-06-duplicate-registration-scenario';
import { runCh07A } from './Scenarios/ch-e2e-07a-missing-channel-scenario';
import { runCh07B } from './Scenarios/ch-e2e-07b-local-server-scenario';
import { runCh07C } from './Scenarios/ch-e2e-07c-unavailable-target-scenario';
import { runCh08 } from './Scenarios/ch-e2e-08-handler-object-scenario';
import { runCh09 } from './Scenarios/ch-e2e-09-port-zero-scenario';
import { runCh10 } from './Scenarios/ch-e2e-10-one-way-scenario';
import { runCh11 } from './Scenarios/ch-e2e-11-channel-name-scenario';
import { runCh12 } from './Scenarios/ch-e2e-12-local-server-selection-scenario';

async function main(): Promise<void> {
  const options = parseClientOptions(process.argv.slice(2));
  const scenarios: Record<string, () => Promise<void>> = {
    'CH-E2E-01': () => runCh01(options),
    'CH-E2E-02': () => runCh02(options),
    'CH-E2E-03': () => runCh03(options),
    'CH-E2E-04A': () => runCh04A(options),
    'CH-E2E-04B': () => runCh04B(options),
    'CH-E2E-04C': () => runCh04C(options),
    'CH-E2E-05': () => runCh05(options),
    'CH-E2E-06': () => runCh06(options),
    'CH-E2E-07A': () => runCh07A(options),
    'CH-E2E-07B': () => runCh07B(options),
    'CH-E2E-07C': () => runCh07C(options),
    'CH-E2E-08': () => runCh08(options),
    'CH-E2E-09': () => runCh09(options),
    'CH-E2E-10': () => runCh10(options),
    'CH-E2E-11': () => runCh11(options),
    'CH-E2E-12': () => runCh12(options)
  };
  const selected = options.scenario.toLowerCase() === 'all'
    ? Object.keys(scenarios)
    : options.scenario.split(',').map((value) => value.trim().toUpperCase()).filter(Boolean);
  for (const id of selected) {
    const scenario = scenarios[id];
    if (scenario === undefined) throw new Error(`Unknown ChannelEgressRouting scenario '${id}'.`);
    await scenario();
    console.log(`scenario ${id} passed`);
  }
  console.log('channel-egress-routing result=passed');
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
