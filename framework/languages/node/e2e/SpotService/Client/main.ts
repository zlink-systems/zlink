import { runSmA1 } from './Scenarios/sm-a1-scenario';
import { runSmA2 } from './Scenarios/sm-a2-scenario';
import { runSmA3 } from './Scenarios/sm-a3-scenario';
import { runSmA4 } from './Scenarios/sm-a4-scenario';
import { runSmA5 } from './Scenarios/sm-a5-scenario';
import { runSmA6 } from './Scenarios/sm-a6-scenario';
import { runSmA7 } from './Scenarios/sm-a7-scenario';
import { runSmA8 } from './Scenarios/sm-a8-scenario';
import { runSmB1 } from './Scenarios/sm-b1-scenario';
import { runSmB2 } from './Scenarios/sm-b2-scenario';
import { runSmB3 } from './Scenarios/sm-b3-scenario';
import { runSmB4 } from './Scenarios/sm-b4-scenario';
import { runSmB5 } from './Scenarios/sm-b5-scenario';
import { runSmB6 } from './Scenarios/sm-b6-scenario';
import { runSmB7 } from './Scenarios/sm-b7-scenario';
import { runSmB8 } from './Scenarios/sm-b8-scenario';
import { runSmB9 } from './Scenarios/sm-b9-scenario';
import { runSmC1 } from './Scenarios/sm-c1-scenario';
import { runSmC2 } from './Scenarios/sm-c2-scenario';
import { runSmC3 } from './Scenarios/sm-c3-scenario';
import { runSmC4 } from './Scenarios/sm-c4-scenario';
import { runSmC5 } from './Scenarios/sm-c5-scenario';
import { runSmD1 } from './Scenarios/sm-d1-scenario';
import { runSmD2 } from './Scenarios/sm-d2-scenario';
import { runSmD3 } from './Scenarios/sm-d3-scenario';
import { runSmD4 } from './Scenarios/sm-d4-scenario';
import { runSmD4A } from './Scenarios/sm-d4a-scenario';
import { runSmD4B } from './Scenarios/sm-d4b-scenario';
import { runSmD5 } from './Scenarios/sm-d5-scenario';
import { runSmD5A } from './Scenarios/sm-d5a-scenario';
import { runSmD6 } from './Scenarios/sm-d6-scenario';
import { runSmD7 } from './Scenarios/sm-d7-scenario';
import { runSmD8 } from './Scenarios/sm-d8-scenario';
import { runSmD9 } from './Scenarios/sm-d9-scenario';
import { runSmD10 } from './Scenarios/sm-d10-scenario';
import { runSmD11 } from './Scenarios/sm-d11-scenario';
import { runSmD12 } from './Scenarios/sm-d12-scenario';
import { runSmD13 } from './Scenarios/sm-d13-scenario';
import { runSmD14 } from './Scenarios/sm-d14-scenario';
import { runSmD15 } from './Scenarios/sm-d15-scenario';
import { runSmD16 } from './Scenarios/sm-d16-scenario';
import { runSmE1 } from './Scenarios/sm-e1-scenario';
import { runSmE2 } from './Scenarios/sm-e2-scenario';
import { runSmE3 } from './Scenarios/sm-e3-scenario';
import { runSmE4 } from './Scenarios/sm-e4-scenario';
import { runSmF1 } from './Scenarios/sm-f1-scenario';
import { runSmF2 } from './Scenarios/sm-f2-scenario';
import { runSmF3 } from './Scenarios/sm-f3-scenario';
import { runSmF4 } from './Scenarios/sm-f4-scenario';
import { runSmF5 } from './Scenarios/sm-f5-scenario';
import { runSmF6 } from './Scenarios/sm-f6-scenario';
import { runSmG1 } from './Scenarios/sm-g1-scenario';
import { prepareSmG2, verifySmG2 } from './Scenarios/sm-g2-scenario';
import { runSmG3 } from './Scenarios/sm-g3-scenario';
import { runSmG4 } from './Scenarios/sm-g4-scenario';
import { runSmQ9 } from './Scenarios/sm-q9-scenario';
import { parseClientOptions } from './Support/client-options';
import { browserE2eConfig, runBrowserE2e } from '../../browser-client-runtime';

async function main(): Promise<void> {
  const options = parseClientOptions(await browserE2eConfig());
  const scenarios: Record<string, () => Promise<void>> = {
    'SM-A1': () => runSmA1(options),
    'SM-A2': () => runSmA2(options),
    'SM-A3': () => runSmA3(options),
    'SM-A4': () => runSmA4(options),
    'SM-A5': () => runSmA5(options),
    'SM-A6': () => runSmA6(options),
    'SM-A7': () => runSmA7(options),
    'SM-A8': () => runSmA8(options),
    'SM-B1': () => runSmB1(options),
    'SM-B2': () => runSmB2(options),
    'SM-B3': () => runSmB3(options),
    'SM-B4': () => runSmB4(options),
    'SM-B5': () => runSmB5(options),
    'SM-B6': () => runSmB6(options),
    'SM-B7': () => runSmB7(options),
    'SM-B8': () => runSmB8(options),
    'SM-B9': () => runSmB9(options),
    'SM-C1': () => runSmC1(options),
    'SM-C2': () => runSmC2(options),
    'SM-C3': () => runSmC3(options),
    'SM-C4': () => runSmC4(options),
    'SM-C5': () => runSmC5(options),
    'SM-D1': () => runSmD1(options),
    'SM-D2': () => runSmD2(options),
    'SM-D3': () => runSmD3(options),
    'SM-D4': () => runSmD4(options),
    'SM-D4A': () => runSmD4A(options),
    'SM-D4B': () => runSmD4B(options),
    'SM-D5': () => runSmD5(options),
    'SM-D5A': () => runSmD5A(options),
    'SM-D6': () => runSmD6(options),
    'SM-D7': () => runSmD7(options),
    'SM-D8': () => runSmD8(options),
    'SM-D9': () => runSmD9(options),
    'SM-D10': () => runSmD10(options),
    'SM-D11': () => runSmD11(options),
    'SM-D12': () => runSmD12(options),
    'SM-D13': () => runSmD13(options),
    'SM-D14': () => runSmD14(options),
    'SM-D15': () => runSmD15(options),
    'SM-D16': () => runSmD16(options),
    'SM-E1': () => runSmE1(options),
    'SM-E2': () => runSmE2(options),
    'SM-E3': () => runSmE3(options),
    'SM-E4': () => runSmE4(options),
    'SM-F1': () => runSmF1(options),
    'SM-F2': () => runSmF2(options),
    'SM-F3': () => runSmF3(options),
    'SM-F4': () => runSmF4(options),
    'SM-F5': () => runSmF5(options),
    'SM-F6': () => runSmF6(options),
    'SM-G1': () => runSmG1(options),
    'SM-G2-PREPARE': () => prepareSmG2(options),
    'SM-G2-VERIFY': () => verifySmG2(options),
    'SM-G3': () => runSmG3(options),
    'SM-G4': () => runSmG4(options),
    'SM-Q9': () => runSmQ9(options)
  };
  const defaultScenarioIds = ['SM-A1', 'SM-A2', 'SM-A3', 'SM-A4', 'SM-A5', 'SM-A6', 'SM-A7', 'SM-A8', 'SM-B1', 'SM-B2', 'SM-B3', 'SM-B4', 'SM-B5', 'SM-B6', 'SM-B7', 'SM-B8', 'SM-B9', 'SM-C1', 'SM-C2', 'SM-C3', 'SM-C4', 'SM-C5', 'SM-D1', 'SM-D2', 'SM-D3', 'SM-D4', 'SM-D5', 'SM-D6', 'SM-D7', 'SM-D9', 'SM-D10', 'SM-D11', 'SM-D12', 'SM-D13', 'SM-D14', 'SM-D15', 'SM-E1', 'SM-E2', 'SM-E3', 'SM-E4', 'SM-F1', 'SM-F2', 'SM-F3', 'SM-F4', 'SM-F5', 'SM-F6'];
  const operationGroups: Record<string, readonly string[]> = {
    'default-batch': defaultScenarioIds,
    'sm-b1-b2-b3-b5': ['SM-B1', 'SM-B2', 'SM-B3', 'SM-B5'],
    'sm-b6': ['SM-B6'],
    'sm-b8': ['SM-B8'],
    'sm-d1-d6': ['SM-D1', 'SM-D2', 'SM-D6'],
    'sm-d3': ['SM-D3'],
    'sm-d4': ['SM-D4'],
    'session-binding-e2e': ['SM-D4A', 'SM-D4B', 'SM-D5', 'SM-D5A'],
    'session-binding-regression': ['SM-D4A', 'SM-D4B', 'SM-D5A'],
    'cross-mesh-actor-dispatch': ['SM-D16'],
    'sm-d5': ['SM-D5'],
    'sm-d7': ['SM-D7'],
    'sm-d8': ['SM-D8'],
    'sm-d9-d11-d13': ['SM-D9', 'SM-D11', 'SM-D13'],
    'sm-d10': ['SM-D10'],
    'sm-d12': ['SM-D12'],
    'sm-d14': ['SM-D14'],
    'sm-c1-c2': ['SM-C1', 'SM-C2'],
    'sm-c3': ['SM-C3'],
    'sm-e4': ['SM-E4'],
    'sm-e1-f4': ['SM-E1', 'SM-F4'],
    'sm-e2-e3': ['SM-E2', 'SM-E3'],
    'sm-a7-a8-c4': ['SM-A7', 'SM-A8', 'SM-C4'],
    'sm-a3-a6-b4-b7': ['SM-A3', 'SM-A6', 'SM-B4', 'SM-B7'],
    'sm-a5': ['SM-A5'],
    'sm-a1-a2-a4-f1-f2': ['SM-A1', 'SM-A2', 'SM-A4', 'SM-F1', 'SM-F2']
  };

  const selectedGroup = operationGroups[options.scenario.toLowerCase()];
  if (options.scenario.toLowerCase() === 'all') {
    for (const scenarioId of defaultScenarioIds) {
      await scenarios[scenarioId]();
    }
  } else if (selectedGroup !== undefined) {
    for (const scenarioId of selectedGroup) {
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

  console.log('spot-service e2e result=passed');
}

void runBrowserE2e('SpotService', main);
