// SF-A1: Store 정상 상태 baseline 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface RouteStatusDto {
  readonly peers: readonly {
    readonly nodeRid: string;
    readonly state: number;
  }[];
  readonly channels: readonly {
    readonly channelName: string;
    readonly isReady: boolean;
    readonly readyTargetCount: number;
  }[];
}

export async function runSfA1(options: ClientOptions): Promise<void> {
  ensure(options.providerBUrl !== undefined, 'SF-A1 requires provider-b-url.');
  await waitForReadyTargets(options.consumerUrl, 2);

  const requestValues = Array.from({ length: 20 }, (_, index) => `sf-a1-${index}`);
  for (const value of requestValues) {
    const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', { value });
    ensure(reply.value === `profile:${value}`, `SF-A1 reply value mismatch for ${value}.`);
    ensure(reply.providerRid === 'api-a' || reply.providerRid === 'api-b', `SF-A1 provider rid mismatch for ${value}.`);
  }

  const providerEvidence = [
    ...await getJson<string[]>(options.providerAUrl, '/evidence'),
    ...await getJson<string[]>(options.providerBUrl, '/evidence')
  ];
  const handledRequests = providerEvidence.filter((entry) =>
    /^profile-request\|.*\|value=sf-a1-(?:[0-9]|1[0-9])\|/.test(entry)
  );
  ensure(handledRequests.length === requestValues.length, `SF-A1 expected 20 provider evidence entries, got ${handledRequests.length}.`);
  console.log('scenario SF-A1 passed');
}

async function waitForReadyTargets(baseUrl: string, count: number): Promise<void> {
  const deadline = Date.now() + 10000;
  let last: RouteStatusDto | undefined;
  while (Date.now() < deadline) {
    last = await getJson<RouteStatusDto>(baseUrl, '/route/status');
    const channel = last.channels.find((candidate) => candidate.channelName === 'profile');
    if (channel?.isReady === true && channel.readyTargetCount === count) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-A1 expected ${count} ready RouteMesh targets, last=${JSON.stringify(last)}`);
}
