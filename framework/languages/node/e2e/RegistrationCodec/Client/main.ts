import { parseClientOptions } from './Support/client-options';
import { runRcA2 } from './Scenarios/AttributeRegistrationScenario';
import { runRcA1 } from './Scenarios/AutoRegistrationScenario';
import { runRcB5 } from './Scenarios/CodecMismatchScenario';
import { runRcA6 } from './Scenarios/InvalidRegistrationScenario';
import { runRcA3 } from './Scenarios/ManualRegistrationScenario';
import { runRcA4 } from './Scenarios/RcA4DiLifecycleScenario';
import { runRcA5 } from './Scenarios/RcA5FilterOrderingScenario';
import { runRcB1 } from './Scenarios/RcB1JsonCodecScenario';
import { runRcB2 } from './Scenarios/RcB2ProtobufCodecScenario';
import { runRcB3 } from './Scenarios/RcB3MessagePackCodecScenario';
import { runRcB4 } from './Scenarios/RcB4CodecCoexistenceScenario';
import { runRCB6 } from './Scenarios/rc-b6-scenario';

async function main(): Promise<void> {
  const options = parseClientOptions(process.argv.slice(2));
  const scenarios: Record<string, () => Promise<void>> = {
    'RC-A1': () => runRcA1(options.serverUrl),
    'RC-A2': () => runRcA2(options.serverUrl),
    'RC-A3': () => runRcA3(options.serverUrl),
    'RC-A4': () => runRcA4(options.serverUrl),
    'RC-A5': () => runRcA5(options.serverUrl),
    'RC-A6': () => runRcA6(options),
    'RC-B1': () => runRcB1(options.serverUrl),
    'RC-B2': () => runRcB2(options.serverUrl),
    'RC-B3': () => runRcB3(options.serverUrl),
    'RC-B4': () => runRcB4(options.serverUrl),
    'RC-B5': () => runRcB5(options.codecRequesterUrl, options.jsonOnlyUrl),
    'RC-B6': () => runRCB6(options.serverUrl)
  };
  if (options.scenario === 'all') {
    for (const run of Object.values(scenarios)) {
      await run();
    }
  } else {
    const run = scenarios[options.scenario];
    if (run === undefined) {
      throw new Error(`Unknown RegistrationCodec scenario '${options.scenario}'.`);
    }
    await run();
  }
  console.log('registration-codec e2e result=passed');
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
