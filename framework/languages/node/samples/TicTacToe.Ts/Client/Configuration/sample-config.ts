import { loadBrowserConfig } from '../../../browser-client-runtime';
type TicTacToeSampleConfig = {
  apiHttpEndpoint: string;
};

async function loadSampleConfig(): Promise<TicTacToeSampleConfig> {
  return await loadBrowserConfig<TicTacToeSampleConfig>();
}

export { loadSampleConfig };
