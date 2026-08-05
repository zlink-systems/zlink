// RL-C2: Crash한 provider를 owner lease 만료 뒤 제외한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { startProvider } from '../Support/managed-provider';
import {
  profileReq,
  waitForProviderTraffic,
  waitPeerAbsent,
  waitPeerPresent,
  waitUntilAvailable,
  waitUntilDown
} from '../Support/resilience-helpers';
import { ensure } from '../Support/scenario-assert';
import type { ScenarioState } from '../Support/scenario-state';

export async function runRlC2(options: ClientOptions, state: ScenarioState): Promise<void> {
  await postJson(options.providerBUrl, '/admin/crash');
  if (state.providerBProcess !== undefined) {
    await state.providerBProcess.waitExited();
    state.providerBProcess = undefined;
  }
  await waitUntilDown(options.providerBUrl);
  await waitPeerAbsent(options.peerLocationUrl, 'api-b');

  for (let i = 0; i < 8; i += 1) {
    const reply = await postJson<ProfileRes>(
      options.consumerUrl,
      '/profile/request/new-client',
      profileReq(`rl-c2-after-crash-${i}`)
    );
    ensure(reply.providerRid === 'api-a', 'RL-C2 request used stale crashed api-b.');
  }

  const restarted = startProvider({
    providerMain: options.providerMain,
    logDir: options.logDir,
    redisEndpoint: options.redisEndpoint,
    redisKeyPrefix: options.redisKeyPrefix,
    name: 'api-b-c2-restored',
    rid: 'api-b',
    httpUrl: options.providerBUrl,
    channelEndpoint: options.providerBChannelEndpoint,
    evidenceFileName: 'api-b-c2-restored.evidence.log'
  });
  await restarted.waitReady();
  state.providerBProcess = restarted;
  await waitUntilAvailable(options.providerBUrl);
  await waitPeerPresent(options.peerLocationUrl, 'api-b');

  await waitForProviderTraffic(options.consumerUrl, 'rl-c2-restored', 'api-b');

  const crashEvidence = await postJson<string[]>(options.providerAUrl, '/evidence/wait', {
    contains: 'marker=rl-c2-after-crash-'
  });
  ensure(
    crashEvidence.some((line) => line.includes('marker=rl-c2-after-crash-')),
    'RL-C2 did not record expected after-crash evidence.'
  );

  const restoreEvidence = await postJson<string[]>(options.providerBUrl, '/evidence/wait', {
    contains: 'profile-request|rid=api-b|value=fast|marker=rl-c2-restored-'
  });
  ensure(
    restoreEvidence.some((line) => line.includes('profile-request|rid=api-b') && line.includes('marker=rl-c2-restored-')),
    'RL-C2 did not record expected restored api-b evidence.'
  );

  console.log('scenario RL-C2 passed');
}
