import { TicTacToeClientScenario } from './tictactoe-client-scenario';
import { loadSampleConfig } from './Configuration/sample-config';
import { runBrowserSample } from '../../browser-client-runtime';
async function main(): Promise<void> {
  const config = await loadSampleConfig();

  await new TicTacToeClientScenario().run(config.apiHttpEndpoint);

  console.log('PASS TicTacToe.Ts');
}

void runBrowserSample('TicTacToe.Ts', main);

export {};
