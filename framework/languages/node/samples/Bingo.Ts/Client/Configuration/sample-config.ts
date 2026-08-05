import { loadBrowserConfig } from '../../../browser-client-runtime';
type BingoSampleConfig = {
  sessionAEndpoint: string;
  sessionBEndpoint: string;
};

async function loadSampleConfig(): Promise<BingoSampleConfig> {
  return await loadBrowserConfig<BingoSampleConfig>();
}

export { loadSampleConfig };
