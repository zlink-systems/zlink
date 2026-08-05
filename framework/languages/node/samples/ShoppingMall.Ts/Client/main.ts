import { ZLinkHttpClient } from '@zlink-systems/http-client';
import { SampleNames } from '../Shared/Configuration/sample-names';
import { loadSampleConfig } from './Configuration/sample-config';
import { ShoppingMallClientScenario } from './shoppingmall-client-scenario';

async function main(): Promise<void> {
  const config = loadSampleConfig();
  const apiA = ZLinkHttpClient.create(config.apiAHttpUrl).timeout(SampleNames.clientTimeout).build();
  const apiB = ZLinkHttpClient.create(config.apiBHttpUrl).timeout(SampleNames.clientTimeout).build();
  try {
    await new ShoppingMallClientScenario().run(
      apiA,
      apiB,
      AbortSignal.timeout(SampleNames.clientTimeout)
    );
  } finally {
    await Promise.allSettled([
      apiA.close(),
      apiB.close()
    ]);
  }
  console.log('shoppingmall=completed');
  console.log('PASS ShoppingMall.Ts');
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
