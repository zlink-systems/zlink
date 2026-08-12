import fs from 'node:fs';
import { runISE2E01 } from './Scenarios/is-e2e-01-scenario';
import { runISE2E02 } from './Scenarios/is-e2e-02-scenario';
import { runISE2E03 } from './Scenarios/is-e2e-03-scenario';
import { runISE2E04 } from './Scenarios/is-e2e-04-scenario';
import { runISE2E05 } from './Scenarios/is-e2e-05-scenario';
import { runISE2E06 } from './Scenarios/is-e2e-06-scenario';
import { runISE2E07 } from './Scenarios/is-e2e-07-scenario';
import { runISE2E08 } from './Scenarios/is-e2e-08-scenario';
import { runISE2E10 } from './Scenarios/is-e2e-10-scenario';
import { runISE2E11 } from './Scenarios/is-e2e-11-scenario';
import { runISE2E12 } from './Scenarios/is-e2e-12-scenario';
import { runISE2E13 } from './Scenarios/is-e2e-13-scenario';
import { runISE2E14 } from './Scenarios/is-e2e-14-scenario';
import { runISE2E15 } from './Scenarios/is-e2e-15-scenario';
import { runISE2E16 } from './Scenarios/is-e2e-16-scenario';
import { runISE2E17 } from './Scenarios/is-e2e-17-scenario';
import { runISE2E18 } from './Scenarios/is-e2e-18-scenario';
import { runISE2E19 } from './Scenarios/is-e2e-19-scenario';
import { runISE2E20 } from './Scenarios/is-e2e-20-scenario';
import { runISE2E21 } from './Scenarios/is-e2e-21-scenario';
import { runISE2E22 } from './Scenarios/is-e2e-22-scenario';
import { runISE2E23 } from './Scenarios/is-e2e-23-scenario';
import { runISE2E24 } from './Scenarios/is-e2e-24-scenario';
import { runISE2E25 } from './Scenarios/is-e2e-25-scenario';
import { runISE2E26 } from './Scenarios/is-e2e-26-scenario';
import { runISE2E27 } from './Scenarios/is-e2e-27-scenario';
import { runISE2E28 } from './Scenarios/is-e2e-28-scenario';
import { runISE2E29 } from './Scenarios/is-e2e-29-scenario';
import { runISE2E30 } from './Scenarios/is-e2e-30-scenario';
import { runISE2E31 } from './Scenarios/is-e2e-31-scenario';
import { runISE2E32 } from './Scenarios/is-e2e-32-scenario';
import { runISE2E33 } from './Scenarios/is-e2e-33-scenario';
import { runISE2E34 } from './Scenarios/is-e2e-34-scenario';
import { runISE2E35 } from './Scenarios/is-e2e-35-scenario';
import { runISE2E36 } from './Scenarios/is-e2e-36-scenario';
import { waitForReady, type InstanceScenarioContext } from './Support/scenario-http';

interface ClientOptions {
  readonly callerUrl: string;
  readonly ownerUrl: string;
  readonly ownerRid: string;
}

type Scenario = (context: InstanceScenarioContext) => Promise<void>;

const scenarios: Readonly<Record<string, Scenario>> = {
  'IS-E2E-01': runISE2E01,
  'IS-E2E-02': runISE2E02,
  'IS-E2E-03': runISE2E03,
  'IS-E2E-04': runISE2E04,
  'IS-E2E-05': runISE2E05,
  'IS-E2E-06': runISE2E06,
  'IS-E2E-07': runISE2E07,
  'IS-E2E-08': runISE2E08,
  'IS-E2E-10': runISE2E10,
  'IS-E2E-11': runISE2E11,
  'IS-E2E-12': runISE2E12,
  'IS-E2E-13': runISE2E13,
  'IS-E2E-14': runISE2E14,
  'IS-E2E-15': runISE2E15,
  'IS-E2E-16': runISE2E16,
  'IS-E2E-17': runISE2E17,
  'IS-E2E-18': runISE2E18,
  'IS-E2E-19': runISE2E19,
  'IS-E2E-20': runISE2E20,
  'IS-E2E-21': runISE2E21,
  'IS-E2E-22': runISE2E22,
  'IS-E2E-23': runISE2E23,
  'IS-E2E-24': runISE2E24,
  'IS-E2E-25': runISE2E25,
  'IS-E2E-26': runISE2E26,
  'IS-E2E-27': runISE2E27,
  'IS-E2E-28': runISE2E28,
  'IS-E2E-29': runISE2E29,
  'IS-E2E-30': runISE2E30,
  'IS-E2E-31': runISE2E31,
  'IS-E2E-32': runISE2E32,
  'IS-E2E-33': runISE2E33,
  'IS-E2E-34': runISE2E34,
  'IS-E2E-35': runISE2E35,
  'IS-E2E-36': runISE2E36
};
const scenarioIds = Object.keys(scenarios);

void main().catch((error: unknown) => {
  console.error(error instanceof Error ? error.stack : String(error));
  process.exitCode = 1;
});

async function main(): Promise<void> {
  const options = readOptions();
  const selectors = process.argv.slice(2).filter((value) => !value.startsWith('--config='));
  const selected = selectors.length === 0 || selectors.some((value) => value.toLowerCase() === 'all')
    ? scenarioIds
    : selectors.flatMap((value) => value.split(','));
  for (const scenarioId of selected) {
    const scenario = scenarios[scenarioId];
    if (scenario === undefined) throw new Error(`Unknown InstanceSpot scenario '${scenarioId}'.`);
    const context: InstanceScenarioContext = { ...options, scenarioId };
    await waitForReady(context);
    await scenario(context);
    console.log(`${scenarioId} PASS`);
  }
}

function readOptions(): ClientOptions {
  const argument = process.argv.find((value) => value.startsWith('--config='));
  if (argument === undefined) throw new Error('InstanceSpot client requires --config=<path>.');
  return JSON.parse(fs.readFileSync(argument.slice('--config='.length), 'utf8')) as ClientOptions;
}
