import { runMonA1 } from './Scenarios/mon-a1-socket-events-scenario';
import { runMonA2 } from './Scenarios/mon-a2-location-runtime-events-scenario';
import { runMonA3 } from './Scenarios/mon-a3-spot-events-scenario';
import { runMonA4 } from './Scenarios/mon-a4-availability-transition-scenario';
import { runMonA5 } from './Scenarios/mon-a5-fixed-kinds-scenario';
import { runMonA6 } from './Scenarios/mon-a6-placement-scenario';
import { runMonA4A } from './Scenarios/mon-a4a-normal-replacement-scenario';
import { runMonA4B } from './Scenarios/mon-a4b-crash-recovery-scenario';
import { runMonB1 } from './Scenarios/mon-b1-kind-filter-scenario';
import { runMonB2 } from './Scenarios/mon-b2-registration-validation-scenario';
import { runMonC1 } from './Scenarios/mon-c1-dispatch-failure-scenario';
import { runMonD1 } from './Scenarios/mon-d1-failure-recovery-scenario';
import { runMonD1B } from './Scenarios/mon-d1b-repeated-restart-scenario';
import { runMonD1A } from './Scenarios/mon-d1a-unregistered-mesh-scenario';
import { parseClientOptions } from './Support/client-options';
import type { ManagedProcess } from './Support/managed-service';

async function main(): Promise<void> {
  const options = parseClientOptions(process.argv.slice(2));
  let serviceBProcess: ManagedProcess | undefined;
  const replaceServiceBProcess = async (start: () => Promise<ManagedProcess>): Promise<void> => {
    const previous = serviceBProcess;
    const replacement = await start();
    await previous?.stop();
    serviceBProcess = replacement;
  };
  const scenarios: Record<string, () => Promise<void>> = {
    'MON-A1': async () => { serviceBProcess = await runMonA1(options); },
    'MON-A2': () => replaceServiceBProcess(() => runMonA2(options)),
    'MON-A3': () => runMonA3(options),
    'MON-A4': () => replaceServiceBProcess(() => runMonA4(options)),
    'MON-A5': () => runMonA5(options),
    'MON-A6': () => runMonA6(options),
    'MON-B1': () => runMonB1(options),
    'MON-B2': () => runMonB2(options),
    'MON-C1': async () => { serviceBProcess = await runMonC1(options); },
    'MON-D1': async () => { serviceBProcess = await runMonD1(options); },
    'MON-A4A': () => replaceServiceBProcess(() => runMonA4A(options)),
    'MON-A4B': async () => { serviceBProcess = await runMonA4B(options); },
    'MON-D1A': () => runMonD1A(options),
    'MON-D1B': async () => { serviceBProcess = await runMonD1B(options); }
  };
  const gaps: Record<string, string> = {};
  const defaultScenarioIds = ['MON-A1', 'MON-A2', 'MON-A3', 'MON-A5', 'MON-A6', 'MON-B1', 'MON-B2', 'MON-C1', 'MON-D1A', 'MON-D1B', 'MON-A4A', 'MON-A4B'];

  try {
    if (options.scenario.toLowerCase() === 'all') {
      for (const scenarioId of defaultScenarioIds) await scenarios[scenarioId]();
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
    await serviceBProcess?.stop();
  }

  console.log('runtime-monitoring e2e result=passed');
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
