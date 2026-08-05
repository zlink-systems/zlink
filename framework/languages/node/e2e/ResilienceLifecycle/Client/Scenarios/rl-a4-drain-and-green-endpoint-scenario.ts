// RL-A4: Rolling과 blue-green update 중 serving target을 유지한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { startProvider } from '../Support/managed-provider';
import {
  profileReq,
  waitPeerEndpointAbsent,
  waitPeerEndpointPresent,
  waitPeerPresent,
  waitUntilAvailable,
  waitUntilDown
} from '../Support/resilience-helpers';
import { ensure } from '../Support/scenario-assert';
import type { ScenarioState } from '../Support/scenario-state';

export async function runRlA4(options: ClientOptions, state: ScenarioState): Promise<void> {
  await postJson(options.providerBUrl, '/admin/drain');
  await waitForWeight(options.providerBUrl, 0);

  const green = startProvider({
    providerMain: options.providerMain,
    logDir: options.logDir,
    redisEndpoint: options.redisEndpoint,
    redisKeyPrefix: options.redisKeyPrefix,
    name: 'api-b-green',
    rid: 'api-b',
    httpUrl: options.providerBGreenUrl,
    channelEndpoint: options.providerBGreenChannelEndpoint,
    evidenceFileName: 'api-b-green.evidence.log'
  });

  try {
    await green.waitReady();

    for (let i = 0; i < 12; i += 1) {
      const reply = await postJson<ProfileRes>(
        options.consumerUrl,
        '/profile/request',
        profileReq(`rl-a4-rolling-${i}`)
      );
      ensure(reply.value === 'profile:fast', 'RL-A4 rolling request returned an unexpected value.');
      ensure(reply.providerRid === 'api-a' || reply.providerRid === 'api-b', 'RL-A4 rolling request used an unexpected provider.');
    }

    if (state.providerBProcess !== undefined) {
      await state.providerBProcess.stop();
    } else {
      await postJson(options.providerBUrl, '/shutdown');
    }
    await waitUntilDown(options.providerBUrl);
    state.providerBProcess = undefined;
    await waitPeerEndpointAbsent(options.peerLocationUrl, options.providerBChannelEndpoint);

    await waitPeerEndpointPresent(options.peerLocationUrl, 'api-b', options.providerBGreenChannelEndpoint);
    await waitPeerPresent(options.peerLocationUrl, 'api-b');
    for (let i = 0; i < 32; i += 1) {
      const reply = await postJson<ProfileRes>(
        options.consumerUrl,
        '/profile/request',
        profileReq(`rl-a4-green-${i}`)
      );
      ensure(reply.value === 'profile:fast', 'RL-A4 green request returned an unexpected value.');
    }
    await waitEvidenceContains(
      options.providerBGreenUrl,
      'profile-request|rid=api-b|value=fast|marker=rl-a4-green-',
      'RL-A4 green provider did not receive green traffic.'
    );
  } finally {
    await green.stop();
  }
  await waitUntilDown(options.providerBGreenUrl);
  await waitPeerEndpointAbsent(options.peerLocationUrl, options.providerBGreenChannelEndpoint);

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
  await waitPeerPresent(options.peerLocationUrl, 'api-b');

  await sendUntilProvider(options.consumerUrl, 'rl-a4-restored', 'api-b', 120);

  console.log('scenario RL-A4 passed');
}

async function waitForWeight(providerUrl: string, expected: number): Promise<void> {
  await postJson(providerUrl, '/admin/weight/wait', { expected });
}

async function sendUntilProvider(
  consumerUrl: string,
  markerPrefix: string,
  expectedProviderRid: string,
  maxAttempts: number
): Promise<void> {
  for (let i = 0; i < maxAttempts; i += 1) {
    const reply = await postJson<ProfileRes>(
      consumerUrl,
      '/profile/request',
      profileReq(`${markerPrefix}-${i}`)
    );
    ensure(reply.value === 'profile:fast', `${markerPrefix} request returned an unexpected value.`);
    if (reply.providerRid === expectedProviderRid) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`Timed out waiting for ${markerPrefix} traffic to reach ${expectedProviderRid}.`);
}

async function waitEvidenceContains(providerUrl: string, contains: string, message: string): Promise<void> {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    const evidence = await getJson<string[]>(providerUrl, '/evidence');
    if (evidence.some((line) => line.includes(contains))) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(message);
}
