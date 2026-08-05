// RM-A2: Manual endpoint로 provider를 연결한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runRmA2(manualConsumerUrl: string, providerAUrl: string): Promise<void> {
  await waitForManualTarget(manualConsumerUrl);
  const reply = await postJson<ProfileRes>(manualConsumerUrl, '/profile/request', { value: 'rm-a2' });
  ensure(reply.value === 'profile:rm-a2', 'RM-A2 reply value mismatch.');
  ensure(reply.providerRid === 'api-a', 'RM-A2 manual endpoint should reach api-a.');
  const evidence = await postJson<string[]>(providerAUrl, '/evidence/wait', { contains: 'value=rm-a2' });
  ensure(evidence.some((line) => line.includes('value=rm-a2')), 'RM-A2 api-a evidence missing.');
  console.log('scenario RM-A2 passed');
}

async function waitForManualTarget(baseUrl: string): Promise<void> {
  let last: ManualRouteStatus | undefined;
  for (let attempt = 0; attempt < 100; attempt += 1) {
    last = await getJson<ManualRouteStatus>(baseUrl, '/route/status');
    const profile = last.channels.find((channel) => channel.channelName === 'profile');
    if (
      profile?.isReady === true
      && profile.readyTargetCount >= 1
      && last.readyPeerCount >= 1
    ) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`RM-A2 manual route did not become ready: ${JSON.stringify(last)}`);
}

interface ManualRouteStatus {
  readonly readyPeerCount: number;
  readonly channels: readonly {
    readonly channelName: string;
    readonly isReady: boolean;
    readonly readyTargetCount: number;
  }[];
}
