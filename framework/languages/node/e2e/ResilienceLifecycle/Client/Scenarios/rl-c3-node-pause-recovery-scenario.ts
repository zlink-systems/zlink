// RL-C3: 정상 restart 뒤 새 lifecycle로 수렴한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { startProvider } from '../Support/managed-provider';
import {
  profileReq,
  waitForProviderTraffic,
  waitPeerPresent,
  waitUntilAvailable,
  waitUntilDown
} from '../Support/resilience-helpers';
import { ensure } from '../Support/scenario-assert';
import type { ScenarioState } from '../Support/scenario-state';

export async function runRlC3(options: ClientOptions, state: ScenarioState): Promise<void> {
  if (state.providerBProcess !== undefined) {
    await state.providerBProcess.stop();
    state.providerBProcess = undefined;
  } else {
    await postJson(options.providerBUrl, '/shutdown');
  }
  await waitUntilDown(options.providerBUrl);

  const during = await postJson<ProfileRes>(
    options.consumerUrl,
    '/profile/request',
    profileReq('rl-c3-during-down')
  );
  ensure(during.providerRid === 'api-a', 'RL-C3 did not use surviving provider during node down.');

  const restarted = startProvider({
    providerMain: options.providerMain,
    logDir: options.logDir,
    redisEndpoint: options.redisEndpoint,
    redisKeyPrefix: options.redisKeyPrefix,
    name: 'api-b-c3-restored',
    rid: 'api-b',
    httpUrl: options.providerBUrl,
    channelEndpoint: options.providerBChannelEndpoint,
    evidenceFileName: 'api-b-c3-restored.evidence.log'
  });
  await restarted.waitReady();
  state.providerBProcess = restarted;
  await waitUntilAvailable(options.providerBUrl);
  await waitPeerPresent(options.peerLocationUrl, 'api-b');

  await waitForProviderTraffic(options.consumerUrl, 'rl-c3-recovered', 'api-b');

  const downEvidence = await postJson<string[]>(options.providerAUrl, '/evidence/wait', {
    contains: 'marker=rl-c3-during-down'
  });
  ensure(
    downEvidence.some((line) => line.includes('marker=rl-c3-during-down')),
    'RL-C3 did not record expected during-down evidence.'
  );

  const recoveredEvidence = await postJson<string[]>(options.providerBUrl, '/evidence/wait', {
    contains: 'profile-request|rid=api-b|value=fast|marker=rl-c3-recovered-'
  });
  ensure(
    recoveredEvidence.some((line) => line.includes('profile-request|rid=api-b') && line.includes('marker=rl-c3-recovered-')),
    'RL-C3 did not record expected recovered api-b evidence.'
  );

  console.log('scenario RL-C3 passed');
}
