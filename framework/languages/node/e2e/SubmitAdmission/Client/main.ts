import fs from 'node:fs';
import { runSAE2E01 } from './Scenarios/sa-e2e-01-scenario';
import { runSAE2E02 } from './Scenarios/sa-e2e-02-scenario';
import { runSAE2E03 } from './Scenarios/sa-e2e-03-scenario';
import { runSAE2E04 } from './Scenarios/sa-e2e-04-scenario';
import { runSAE2E05 } from './Scenarios/sa-e2e-05-scenario';
import { runSAE2E06 } from './Scenarios/sa-e2e-06-scenario';
import { runSAE2E07 } from './Scenarios/sa-e2e-07-scenario';
import { runSAE2E08 } from './Scenarios/sa-e2e-08-scenario';
import { runSAE2E09 } from './Scenarios/sa-e2e-09-scenario';
import { runSAE2E10 } from './Scenarios/sa-e2e-10-scenario';
import { runSAE2E11 } from './Scenarios/sa-e2e-11-scenario';
import { runSAE2E12 } from './Scenarios/sa-e2e-12-scenario';
import { runSAE2E13 } from './Scenarios/sa-e2e-13-scenario';
import { runSAE2E14 } from './Scenarios/sa-e2e-14-scenario';
import { runSAE2E15 } from './Scenarios/sa-e2e-15-scenario';
import { runSAE2E16 } from './Scenarios/sa-e2e-16-scenario';
import { runSAE2E17 } from './Scenarios/sa-e2e-17-scenario';
import { runSAE2E18 } from './Scenarios/sa-e2e-18-scenario';
import { runSAE2E19 } from './Scenarios/sa-e2e-19-scenario';
import { runSAE2E20 } from './Scenarios/sa-e2e-20-scenario';
import type { SubmitScenarioContext } from './Support/scenario-http';

type Scenario = (context: SubmitScenarioContext) => Promise<void>;
interface ClientOptions {
  readonly callerUrl: string;
  readonly targetUrl: string;
  readonly publisherUrl: string;
  readonly callerRid: string;
  readonly targetRid: string;
  readonly evidenceFile: string;
}

const scenarios: Readonly<Record<string, Scenario>> = {
  'SA-E2E-01': runSAE2E01,
  'SA-E2E-02': runSAE2E02,
  'SA-E2E-03': runSAE2E03,
  'SA-E2E-04': runSAE2E04,
  'SA-E2E-05': runSAE2E05,
  'SA-E2E-06': runSAE2E06,
  'SA-E2E-07': runSAE2E07,
  'SA-E2E-08': runSAE2E08,
  'SA-E2E-09': runSAE2E09,
  'SA-E2E-10': runSAE2E10,
  'SA-E2E-11': runSAE2E11,
  'SA-E2E-12': runSAE2E12,
  'SA-E2E-13': runSAE2E13,
  'SA-E2E-14': runSAE2E14,
  'SA-E2E-15': runSAE2E15,
  'SA-E2E-16': runSAE2E16,
  'SA-E2E-17': runSAE2E17,
  'SA-E2E-18': runSAE2E18,
  'SA-E2E-19': runSAE2E19,
  'SA-E2E-20': runSAE2E20
};

void main().catch((error: unknown) => {
  console.error(error instanceof Error ? error.stack : String(error));
  process.exitCode = 1;
});

async function main(): Promise<void> {
  const options = readOptions();
  const selectors = process.argv.slice(2).filter((value) => !value.startsWith('--config='));
  if (selectors.length === 0) throw new Error('SubmitAdmission client requires at least one scenario selector.');
  for (const scenarioId of selectors) {
    const scenario = scenarios[scenarioId];
    if (scenario === undefined) throw new Error('Unknown SubmitAdmission scenario ' + scenarioId + '.');
    await scenario({ ...options, scenarioId });
  }
}

function readOptions(): ClientOptions {
  const argument = process.argv.find((value) => value.startsWith('--config='));
  if (argument === undefined) throw new Error('SubmitAdmission client requires --config=<path>.');
  return JSON.parse(fs.readFileSync(argument.slice('--config='.length), 'utf8')) as ClientOptions;
}

