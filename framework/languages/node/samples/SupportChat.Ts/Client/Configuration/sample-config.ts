import { loadBrowserConfig } from '../../../browser-client-runtime';

type SupportChatClientConfig = { sessionStreamEndpoint: string };

async function loadSampleConfig(): Promise<SupportChatClientConfig> {
  return await loadBrowserConfig<SupportChatClientConfig>();
}

export { loadSampleConfig };
export type { SupportChatClientConfig };
