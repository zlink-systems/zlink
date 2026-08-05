// RL-A2: 다른 endpoint의 replacement로 전환한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { startProvider } from '../Support/managed-provider';
import {
  waitForProviderTraffic,
  waitPeerEndpointPresent,
  waitUntilAvailable,
  waitUntilDown
} from '../Support/resilience-helpers';
import type { ScenarioState } from '../Support/scenario-state';

export async function runRlA2(options: ClientOptions, state: ScenarioState): Promise<void> {
  if (state.providerBProcess !== undefined) {
    await state.providerBProcess.stop();
  } else {
    await postJson(options.providerBUrl, '/shutdown');
  }
  await waitUntilDown(options.providerBUrl);
  state.providerBProcess = undefined;

  const remapped = startProvider({
    providerMain: options.providerMain,
    logDir: options.logDir,
    redisEndpoint: options.redisEndpoint,
    redisKeyPrefix: options.redisKeyPrefix,
    name: 'api-b-rescheduled',
    rid: 'api-b',
    httpUrl: options.providerBRemapUrl,
    channelEndpoint: options.providerBRemapChannelEndpoint,
    evidenceFileName: 'api-b-rescheduled.evidence.log'
  });
  try {
    await remapped.waitReady();
    await waitPeerEndpointPresent(options.peerLocationUrl, 'api-b', options.providerBRemapChannelEndpoint);
    await waitForProviderTraffic(options.consumerUrl, 'rl-a2-rescheduled', 'api-b');
    await postJson<string[]>(options.providerBRemapUrl, '/evidence/wait', { contains: 'marker=rl-a2-rescheduled-' });
  } finally {
    await remapped.stop();
  }
  await waitUntilDown(options.providerBRemapUrl);

  const restored = startProvider({
    providerMain: options.providerMain,
    logDir: options.logDir,
    redisEndpoint: options.redisEndpoint,
    redisKeyPrefix: options.redisKeyPrefix,
    name: 'api-b-original-restored',
    rid: 'api-b',
    httpUrl: options.providerBUrl,
    channelEndpoint: options.providerBChannelEndpoint,
    evidenceFileName: 'api-b-original-restored.evidence.log'
  });
  await restored.waitReady();
  state.providerBProcess = restored;
  await waitUntilAvailable(options.providerBUrl);
  await waitPeerEndpointPresent(options.peerLocationUrl, 'api-b', options.providerBChannelEndpoint);
  await waitForProviderTraffic(options.consumerUrl, 'rl-a2-original-restored', 'api-b');
  await postJson<string[]>(options.providerBUrl, '/evidence/wait', { contains: 'marker=rl-a2-original-restored-' });

  console.log('scenario RL-A2 passed');
}
