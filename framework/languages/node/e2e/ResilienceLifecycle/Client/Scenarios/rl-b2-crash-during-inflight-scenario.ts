// RL-B2: In-flight handler 중 provider crash를 처리한다 시나리오를 검증한다.
import type { ProfileRes, TimeoutRes } from '../../Shared/messages';
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

export async function runRlB2(options: ClientOptions, state: ScenarioState): Promise<void> {
  await postJson(options.providerAUrl, '/admin/drain');
  await waitForWeight(options.providerAUrl, 0);
  await postJson(options.providerBUrl, '/admin/fault/crash-on-slow-start');

  const inFlight = await startSlowRequestOnApiB(options);
  if (state.providerBProcess !== undefined) {
    await state.providerBProcess.waitExited();
    state.providerBProcess = undefined;
  }
  await waitUntilDown(options.providerBUrl);

  const failed = await inFlight;
  ensure(failed.status !== 200 || failed.timedOut, 'RL-B2 in-flight request unexpectedly completed after provider crash.');

  await postJson(options.providerAUrl, '/admin/restore');
  await waitForWeight(options.providerAUrl, 100);

  await waitPeerAbsent(options.peerLocationUrl, 'api-b');
  const followUp = await postJson<ProfileRes>(
    options.consumerUrl,
    '/profile/request/new-client',
    profileReq('rl-b2-after-crash')
  );
  ensure(followUp.providerRid === 'api-a', 'RL-B2 surviving provider traffic failed.');

  const restarted = startProvider({
    providerMain: options.providerMain,
    logDir: options.logDir,
    redisEndpoint: options.redisEndpoint,
    redisKeyPrefix: options.redisKeyPrefix,
    name: 'api-b-b2-restored',
    rid: 'api-b',
    httpUrl: options.providerBUrl,
    channelEndpoint: options.providerBChannelEndpoint,
    evidenceFileName: 'api-b-b2-restored.evidence.log'
  });
  await restarted.waitReady();
  state.providerBProcess = restarted;
  await waitUntilAvailable(options.providerBUrl);
  await waitPeerPresent(options.peerLocationUrl, 'api-b');

  await waitForProviderTraffic(options.consumerUrl, 'rl-b2-restored', 'api-b');
  await postJson<string[]>(options.providerBUrl, '/evidence/wait', { contains: 'marker=rl-b2-restored-' });

  console.log('scenario RL-B2 passed');
}

async function waitForWeight(providerUrl: string, expected: number): Promise<void> {
  await postJson(providerUrl, '/admin/weight/wait', { expected });
}

async function startSlowRequestOnApiB(options: ClientOptions): Promise<Promise<TimeoutRes>> {
  for (let attempt = 0; attempt < 12; attempt += 1) {
    const marker = `rl-b2-slow-${attempt}-${Date.now()}-${Math.random().toString(16).slice(2)}`;
    const inFlight = postJson<TimeoutRes>(
      options.consumerUrl,
      '/profile/request/timeout/10000',
      profileRequestWithValue('slow', marker)
    );
    try {
      await postJson<string[]>(options.providerBUrl, '/evidence/wait', {
        contains: `profile-start|rid=api-b|value=slow|marker=${marker}`,
        timeoutMilliseconds: 1000
      });
      return inFlight;
    } catch {
      const result = await inFlight;
      ensure(result.status === 200 && !result.timedOut, 'RL-B2 selection probe failed before api-b accepted the request.');
    }
  }
  throw new Error('RL-B2 could not route a slow request to api-b.');
}

function profileRequestWithValue(value: string, marker: string): { readonly value: string; readonly marker: string } {
  return { value, marker };
}
