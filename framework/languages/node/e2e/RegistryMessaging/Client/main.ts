import { parseClientOptions } from './Support/client-options';
import { runRmA1 } from './Scenarios/rm-a1-discovery-request-scenario';
import { runRmA2 } from './Scenarios/rm-a2-manual-endpoint-scenario';
import { runRmA3 } from './Scenarios/rm-a3-object-client-connection-scenario';
import { runRmA4 } from './Scenarios/rm-a4-same-rid-failover-scenario';
import { runRmA6 } from './Scenarios/rm-a6-multiple-channels-scenario';
import { runRmB1 } from './Scenarios/rm-b1-scale-out-scenario';
import { runRmB2 } from './Scenarios/rm-b2-scale-in-scenario';
import { runRmB3 } from './Scenarios/rm-b3-provider-crash-failover-scenario';
import { runRmC1 } from './Scenarios/rm-c1-request-send-scenario';
import { runRmC2 } from './Scenarios/rm-c2-targeted-route-scenario';
import { runRmC3 } from './Scenarios/rm-c3-multi-provider-distribution-scenario';
import { runRmC4 } from './Scenarios/rm-c4-timeout-isolation-scenario';
import { runRmC5 } from './Scenarios/rm-c5-missing-packet-scenario';
import { runRmC7 } from './Scenarios/rm-c7-weighted-provider-scenario';
import { runRmC8 } from './Scenarios/rm-c8-payload-round-trip-scenario';
import { runRmC9 } from './Scenarios/rm-c9-backpressure-scenario';

async function main(): Promise<void> {
  const options = parseClientOptions(process.argv.slice(2));
  const scenarios: Record<string, () => Promise<void>> = {
    'RM-A1': () => runRmA1(options.locationConsumerUrl, options.providerAUrl, options.providerBUrl),
    'RM-A2': () => runRmA2(options.manualConsumerUrl ?? options.directConsumerUrl, options.providerAUrl),
    'RM-A3': () => runRmA3(options),
    'RM-A4': () => runRmA4(options),
    'RM-A6': () => runRmA6(options.providerAUrl, options.providerBUrl, options.workflowUrl),
    'RM-B1': () => runRmB1(options),
    'RM-B2': () => runRmB2(options),
    'RM-B3': () => runRmB3(options),
    'RM-C1': () => runRmC1(options.providerAUrl, options.providerBUrl),
    'RM-C2': () => runRmC2(options.providerAUrl, options.providerBUrl),
    'RM-C3': () => runRmC3(options.directConsumerUrl, options.providerAUrl, options.providerBUrl),
    'RM-C4': () => runRmC4(options.locationConsumerUrl, options.providerAUrl, options.providerBUrl),
    'RM-C5': () => runRmC5(options.locationConsumerUrl, options.providerAUrl, options.providerBUrl),
    'RM-C7': () => runRmC7(options),
    'RM-C8': () => runRmC8(options.singleConsumerUrl, options.directConsumerUrl, options.providerAUrl),
    'RM-C9': () => runRmC9(options.backpressureConsumerUrl, options.providerAUrl)
  };
  const defaultScenarioIds = [
    'RM-A1',
    'RM-A2',
    'RM-A4',
    'RM-A6',
    'RM-B1',
    'RM-B2',
    'RM-B3',
    'RM-C1',
    'RM-C2',
    'RM-C3',
    'RM-C4',
    'RM-C5',
    'RM-C7',
    'RM-C8',
    'RM-C9'
  ];

  if (options.scenario.toLowerCase() === 'all') {
    for (const scenarioId of defaultScenarioIds) {
      await scenarios[scenarioId]();
    }
  } else {
    const selected = options.scenario.toUpperCase();
    const scenario = scenarios[selected];
    if (scenario === undefined) {
      throw new Error(`Unknown scenario '${options.scenario}'.`);
    }
    await scenario();
  }

  console.log('registry-messaging e2e result=passed');
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
