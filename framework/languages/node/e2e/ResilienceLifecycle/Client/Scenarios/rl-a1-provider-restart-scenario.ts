// RL-A1: 같은 endpoint에서 server를 재시작한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { startProvider } from '../Support/managed-provider';
import {
  profileReq,
  waitForProviderTraffic,
  waitPeerAbsent,
  waitPeerPresent,
  waitUntilDown
} from '../Support/resilience-helpers';
import { ensure } from '../Support/scenario-assert';
import type { ScenarioState } from '../Support/scenario-state';
import type { ProfileRes } from '../../Shared/messages';

export async function runRlA1(options: ClientOptions, state: ScenarioState): Promise<void> {
  if (state.providerBProcess !== undefined) {
    await state.providerBProcess.stop();
  } else {
    await postJson(options.providerBUrl, '/shutdown');
  }
  await waitUntilDown(options.providerBUrl);
  await waitPeerAbsent(options.peerLocationUrl, 'api-b');
  state.providerBProcess = undefined;

  for (let i = 0; i < 12; i += 1) {
    const marker = `rl-a1-down-${i}`;
    const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', profileReq(marker));
    ensure(reply.providerRid === 'api-a', 'RL-A1 request during api-b restart did not use surviving provider.');
  }
  await postJson<string[]>(options.providerAUrl, '/evidence/wait', { contains: 'marker=rl-a1-down-' });

  const restarted = startProvider({
    providerMain: options.providerMain,
    logDir: options.logDir,
    redisEndpoint: options.redisEndpoint,
    redisKeyPrefix: options.redisKeyPrefix,
    name: 'api-b-restart',
    rid: 'api-b',
    httpUrl: options.providerBUrl,
    channelEndpoint: options.providerBChannelEndpoint,
    evidenceFileName: 'api-b-restart.evidence.log'
  });
  await restarted.waitReady();
  state.providerBProcess = restarted;
  await waitPeerPresent(options.peerLocationUrl, 'api-b');

  await waitForProviderTraffic(options.consumerUrl, 'rl-a1-restored', 'api-b');
  await postJson<string[]>(options.providerBUrl, '/evidence/wait', { contains: 'marker=rl-a1-restored-' });

  console.log('scenario RL-A1 passed');
}
