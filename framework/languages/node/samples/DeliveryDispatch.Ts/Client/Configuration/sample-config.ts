type DeliveryDispatchClientConfig = {
  dispatchApiHttpUrl: string;
  sessionStreamEndpoint: string;
  courierStreamEndpoint: string;
};

async function loadSampleConfig(): Promise<DeliveryDispatchClientConfig> {
  return await loadBrowserConfig<DeliveryDispatchClientConfig>();
}

export {
  loadSampleConfig
};

export type {
  DeliveryDispatchClientConfig
};
import { loadBrowserConfig } from '../../../browser-client-runtime';
