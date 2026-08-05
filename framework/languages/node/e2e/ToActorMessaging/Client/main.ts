import { browserE2eConfig, runBrowserE2e } from '../../browser-client-runtime';
import { runTaA1 } from './Scenarios/ta-a1-bound-no-bind-scenario';
import { runTaA2 } from './Scenarios/ta-a2-unbound-no-bind-scenario';
import { runTaA3 } from './Scenarios/ta-a3-no-bind-then-bind-scenario';
import { runTaA4 } from './Scenarios/ta-a4-disconnect-destroy-scenario';
import { runTaB1 } from './Scenarios/ta-b1-missing-actor-scenario';
import { runTaB2 } from './Scenarios/ta-b2-stale-ref-scenario';
import { runTaB3 } from './Scenarios/ta-b3-route-disconnected-scenario';
import { parseClientOptions } from './Support/client-options';
import type { ClientOptions } from './Support/client-options';

const scenarios: Record<string, (options: ClientOptions) => Promise<void>> = {
  'TA-A1': runTaA1,
  'TA-A2': runTaA2,
  'TA-A3': runTaA3,
  'TA-A4': runTaA4,
  'TA-B1': runTaB1,
  'TA-B2': runTaB2,
  'TA-B3': runTaB3
};

async function main(): Promise<void> {
  const options = parseClientOptions(await browserE2eConfig());
  if (options.scenario === 'all') {
    for (const scenario of Object.values(scenarios)) await scenario(options);
  } else {
    const scenario = scenarios[options.scenario];
    if (scenario === undefined) throw new Error(`Unknown scenario '${options.scenario}'.`);
    await scenario(options);
  }
  console.log('to-actor-messaging e2e result=passed');
}

void runBrowserE2e('ToActorMessaging', main);
