// RL-B3: Graceful Shutdown 뒤 topology에서 제거한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { startProvider } from '../Support/managed-provider';
import {
  profileReq,
  waitPeerAbsent,
  waitPeerPresent,
  waitUntilAvailable,
  waitUntilDown
} from '../Support/resilience-helpers';
import { ensure } from '../Support/scenario-assert';
import type { ScenarioState } from '../Support/scenario-state';

export async function runRlB3(options: ClientOptions, state: ScenarioState): Promise<void> {
  const before = await postJson<ProfileRes>(
    options.consumerUrl,
    '/profile/request',
    profileReq(`rl-b3-before-${Date.now()}`)
  );
  ensure(before.providerRid === 'api-a' || before.providerRid === 'api-b', 'RL-B3 pre-shutdown request failed.');

  if (state.providerBProcess !== undefined) {
    await state.providerBProcess.stop();
    state.providerBProcess = undefined;
  } else {
    await postJson(options.providerBUrl, '/shutdown');
  }
  await waitUntilDown(options.providerBUrl);
  await waitPeerAbsent(options.peerLocationUrl, 'api-b');

  for (let i = 0; i < 12; i += 1) {
    const after = await postJson<ProfileRes>(
      options.consumerUrl,
      '/profile/request',
      profileReq(`rl-b3-after-${i}`)
    );
    ensure(after.providerRid === 'api-a', 'RL-B3 request after graceful shutdown used stale api-b.');
  }
  await postJson<string[]>(options.providerAUrl, '/evidence/wait', { contains: 'marker=rl-b3-after-' });

  const restarted = startProvider({
    providerMain: options.providerMain,
    logDir: options.logDir,
    redisEndpoint: options.redisEndpoint,
    redisKeyPrefix: options.redisKeyPrefix,
    name: 'api-b-b3-restored',
    rid: 'api-b',
    httpUrl: options.providerBUrl,
    channelEndpoint: options.providerBChannelEndpoint,
    evidenceFileName: 'api-b-b3-restored.evidence.log'
  });
  await restarted.waitReady();
  state.providerBProcess = restarted;
  await waitUntilAvailable(options.providerBUrl);
  await waitPeerPresent(options.peerLocationUrl, 'api-b');

  console.log('scenario RL-B3 passed');
}
