// RL-A5: Provider lifecycle을 반복해도 current target에 수렴한다 시나리오를 검증한다.
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
import type { ProfileRes } from '../../Shared/messages';

export async function runRlA5(options: ClientOptions, state: ScenarioState): Promise<void> {
  for (let cycle = 0; cycle < 3; cycle += 1) {
    await stopProviderB(options, state);
    state.providerBProcess = undefined;
    await waitForSurvivingProvider(options.consumerUrl, cycle);

    for (let i = 0; i < 4; i += 1) {
      const marker = `rl-a5-down-${cycle}-${i}`;
      const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', profileReq(marker));
      ensure(reply.providerRid === 'api-a', 'RL-A5 down window did not converge to api-a.');
    }
    await postJson<string[]>(options.providerAUrl, '/evidence/wait', { contains: `marker=rl-a5-down-${cycle}-` });

    const restarted = startProvider({
      providerMain: options.providerMain,
      logDir: options.logDir,
      redisEndpoint: options.redisEndpoint,
      redisKeyPrefix: options.redisKeyPrefix,
      name: `api-b-flap-${cycle}`,
      rid: 'api-b',
      httpUrl: options.providerBUrl,
      channelEndpoint: options.providerBChannelEndpoint,
      evidenceFileName: `api-b-flap-${cycle}.evidence.log`
    });
    await restarted.waitReady();
    state.providerBProcess = restarted;
    await waitUntilAvailable(options.providerBUrl);
    await waitPeerPresent(options.peerLocationUrl, 'api-b');

    await waitForProviderTraffic(options.consumerUrl, `rl-a5-up-${cycle}`, 'api-b');
    await postJson<string[]>(options.providerBUrl, '/evidence/wait', {
      contains: `profile-request|rid=api-b|value=fast|marker=rl-a5-up-${cycle}-`
    });
  }

  console.log('scenario RL-A5 passed');
}

async function stopProviderB(options: ClientOptions, state: ScenarioState): Promise<void> {
  if (state.providerBProcess !== undefined) {
    await state.providerBProcess.stop();
    await waitUntilDown(options.providerBUrl);
    return;
  }
  await postJson(options.providerBUrl, '/shutdown');
  await waitUntilDown(options.providerBUrl);
}

async function waitForSurvivingProvider(consumerUrl: string, cycle: number): Promise<void> {
  const deadline = Date.now() + 15000;
  let consecutiveApiA = 0;
  let attempt = 0;
  while (Date.now() < deadline) {
    const marker = `rl-a5-converge-${cycle}-${attempt++}`;
    const reply = await postJson<ProfileRes>(consumerUrl, '/profile/request', profileReq(marker));
    ensure(reply.value === 'profile:fast', 'RL-A5 convergence probe returned an unexpected value.');
    consecutiveApiA = reply.providerRid === 'api-a' ? consecutiveApiA + 1 : 0;
    if (consecutiveApiA >= 4) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error('RL-A5 down window did not converge to api-a.');
}
