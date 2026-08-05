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

async function main(): Promise<void> {
  const options = parseClientOptions(process.argv.slice(2));
  if (!['SF-A1', 'SF-A2', 'SF-B1', 'SF-B2', 'SF-C1', 'SF-C2', 'SF-D1', 'SF-D2', 'SF-D3', 'SF-E1', 'all'].includes(options.scenario)) {
    throw new Error(`Unsupported scenario '${options.scenario}'.`);
  }
  if (options.scenario === 'SF-A1' || options.scenario === 'all') {
    await runSfA1(options);
  }
  if (options.scenario === 'SF-A2') {
    await runSfA2(options);
  }
  if (options.scenario === 'SF-B1') {
    await runSfB1(options);
  }
  if (options.scenario === 'SF-B2') {
    await runSfB2(options);
  }
  if (options.scenario === 'SF-C1') {
    await runSfC1(options);
  }
  if (options.scenario === 'SF-C2') {
    await runSfC2(options);
  }
  if (options.scenario === 'SF-D1') {
    await runSfD1(options);
  }
  if (options.scenario === 'SF-D2') {
    await runSfD2(options);
  }
  if (options.scenario === 'SF-D3') {
    await runSfD3(options);
  }
  if (options.scenario === 'SF-E1') {
    await runSfE1(options);
  }
  console.log('store-failure-recovery scenario result=passed');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
