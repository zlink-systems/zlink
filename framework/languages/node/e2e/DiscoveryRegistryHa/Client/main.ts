import { parseClientOptions } from './Support/client-options';
import { runSfA1 } from './Scenarios/SfA1BaselineScenario';
import { runSfA2 } from './Scenarios/SfA2PollingFallbackScenario';
import { runSfB1 } from './Scenarios/SfB1StoreOutageScenario';
import { runSfB2 } from './Scenarios/SfB2StoreFailureGraceScenario';
import { runSfC1 } from './Scenarios/SfC1ProviderCrashScenario';
import { runSfC2 } from './Scenarios/SfC2GracefulShutdownScenario';
import { runSfD1 } from './Scenarios/SfD1ShortOutageRecoveryScenario';
import { runSfD2 } from './Scenarios/SfD2LongOutageRecoveryScenario';
import { runSfD3 } from './Scenarios/SfD3RuntimeStatusTransitionScenario';
import { runSfE1 } from './Scenarios/SfE1StoreResponseDelayScenario';
import { runSFB3 } from './Scenarios/sf-b3-scenario';
import { runSFC3 } from './Scenarios/sf-c3-scenario';
import { runSFC4 } from './Scenarios/sf-c4-scenario';
import { runSFC5 } from './Scenarios/sf-c5-scenario';
import { runSFC5A } from './Scenarios/sf-c5a-scenario';
import { runSFF1 } from './Scenarios/sf-f1-scenario';
import { runSFF2 } from './Scenarios/sf-f2-scenario';
import { runSFF3 } from './Scenarios/sf-f3-scenario';
import { runSFF4 } from './Scenarios/sf-f4-scenario';
import { runSFF5 } from './Scenarios/sf-f5-scenario';
import { runSFF6 } from './Scenarios/sf-f6-scenario';
import { runSFF7 } from './Scenarios/sf-f7-scenario';
import { runSFF8 } from './Scenarios/sf-f8-scenario';
import { runSFF9 } from './Scenarios/sf-f9-scenario';
import { runSFF10 } from './Scenarios/sf-f10-scenario';
import { runSFF11 } from './Scenarios/sf-f11-scenario';
import { runSFG1 } from './Scenarios/sf-g1-scenario';
import { runSFG2 } from './Scenarios/sf-g2-scenario';

async function main(): Promise<void> {
  const options = parseClientOptions(process.argv.slice(2));
  if (!['SF-A1', 'SF-A2', 'SF-B1', 'SF-B2', 'SF-B3', 'SF-C1', 'SF-C2', 'SF-C3', 'SF-C4', 'SF-C5', 'SF-C5A', 'SF-D1', 'SF-D2', 'SF-D3', 'SF-E1', 'SF-F1', 'SF-F2', 'SF-F3', 'SF-F4', 'SF-F5', 'SF-F6', 'SF-F7', 'SF-F8', 'SF-F9', 'SF-F10', 'SF-F11', 'SF-G1', 'SF-G2', 'all'].includes(options.scenario)) {
    throw new Error(`Unsupported scenario '${options.scenario}'.`);
  }
  const scenarios: Readonly<Record<string, () => Promise<void>>> = {
    'SF-A1': () => runSfA1(options), 'SF-A2': () => runSfA2(options),
    'SF-B1': () => runSfB1(options), 'SF-B2': () => runSfB2(options), 'SF-B3': () => runSFB3(options),
    'SF-C1': () => runSfC1(options), 'SF-C2': () => runSfC2(options), 'SF-C3': () => runSFC3(options),
    'SF-C4': () => runSFC4(options), 'SF-C5': () => runSFC5(options), 'SF-C5A': () => runSFC5A(options),
    'SF-D1': () => runSfD1(options), 'SF-D2': () => runSfD2(options), 'SF-D3': () => runSfD3(options),
    'SF-E1': () => runSfE1(options),
    'SF-F1': () => runSFF1(options), 'SF-F2': () => runSFF2(options), 'SF-F3': () => runSFF3(options),
    'SF-F4': () => runSFF4(options), 'SF-F5': () => runSFF5(options), 'SF-F6': () => runSFF6(options),
    'SF-F7': () => runSFF7(options), 'SF-F8': () => runSFF8(options), 'SF-F9': () => runSFF9(options),
    'SF-F10': () => runSFF10(options), 'SF-F11': () => runSFF11(options),
    'SF-G1': () => runSFG1(options), 'SF-G2': () => runSFG2(options)
  };
  const selected = options.scenario.toLowerCase() === 'all'
    ? Object.keys(scenarios)
    : [options.scenario.toUpperCase()];
  for (const scenarioId of selected) {
    const scenario = scenarios[scenarioId];
    if (scenario === undefined) throw new Error('Unsupported scenario ' + scenarioId + '.');
    await scenario();
  }
  console.log('store-failure-recovery scenario result=passed');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
