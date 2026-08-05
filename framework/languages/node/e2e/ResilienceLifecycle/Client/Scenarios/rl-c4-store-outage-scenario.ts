// RL-C4: Location Store restart 중 existing messaging을 유지한다 시나리오를 검증한다.
import { spawn } from 'node:child_process';
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import {
  profileReq,
  waitPeerPresent
} from '../Support/resilience-helpers';
import { ensure } from '../Support/scenario-assert';
import type { ScenarioState } from '../Support/scenario-state';

export async function runRlC4(options: ClientOptions, _state: ScenarioState): Promise<void> {
  const before = await postJson<ProfileRes>(
    options.consumerUrl,
    '/profile/request',
    profileReq('rl-c4-before-outage')
  );
  ensure(before.value === 'profile:fast', 'RL-C4 request failed before store outage.');

  await docker('pause', options.redisContainer);
  try {
    const during = await postJson<ProfileRes>(
      options.consumerUrl,
      '/profile/request',
      profileReq('rl-c4-during-outage')
    );
    ensure(during.value === 'profile:fast', 'RL-C4 existing channel failed during store outage.');
    await waitForEitherEvidence(options, 'marker=rl-c4-during-outage', 'RL-C4 during-outage evidence missing.');
  } finally {
    await docker('unpause', options.redisContainer);
  }

  await waitForEitherEvidence(options, 'marker=rl-c4-before-outage', 'RL-C4 before-outage evidence missing.');
  await waitPeerPresent(options.peerLocationUrl, 'api-a');

  const after = await postJson<ProfileRes>(
    options.consumerUrl,
    '/profile/request/new-client',
    profileReq('rl-c4-after-recovery')
  );
  ensure(after.value === 'profile:fast', 'RL-C4 follow-up request failed after store recovery.');

  await waitForEitherEvidence(options, 'marker=rl-c4-after-recovery', 'RL-C4 after-recovery evidence missing.');

  console.log('scenario RL-C4 passed');
}

async function waitForEitherEvidence(options: ClientOptions, contains: string, message: string): Promise<void> {
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    const snapshots = await Promise.allSettled([
      getJson<string[]>(options.providerAUrl, '/evidence'),
      getJson<string[]>(options.providerBUrl, '/evidence')
    ]);
    if (snapshots.some((snapshot) =>
      snapshot.status === 'fulfilled' && snapshot.value.some((line) => line.includes(contains)))) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  ensure(false, message);
}

async function docker(verb: 'pause' | 'unpause', container: string): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    const child = spawn('docker', [verb, container], { stdio: 'ignore' });
    child.once('exit', (code) => {
      if (code === 0) {
        resolve();
      } else {
        reject(new Error(`docker ${verb} ${container} failed with exit code ${code}`));
      }
    });
    child.once('error', reject);
  });
}
