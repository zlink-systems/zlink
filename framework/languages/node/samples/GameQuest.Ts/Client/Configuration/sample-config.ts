type GameQuestClientConfig = {
  apiAHttpUrl: string;
  apiBHttpUrl: string;
  apiAStreamEndpoint: string;
  apiBStreamEndpoint: string;
  missionAHttpUrl: string;
  missionBHttpUrl: string;
};

async function loadSampleConfig(): Promise<GameQuestClientConfig> {
  return await loadBrowserConfig<GameQuestClientConfig>();
}

export { loadSampleConfig };
export type { GameQuestClientConfig };
import { loadBrowserConfig } from '../../../browser-client-runtime';
