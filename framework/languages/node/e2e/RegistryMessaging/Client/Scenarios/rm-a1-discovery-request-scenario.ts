// RM-A1: Location Store로 provider를 찾는다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runRmA1(locationConsumerUrl: string, providerAUrl: string, providerBUrl: string): Promise<void> {
  await waitForReadyTargets(locationConsumerUrl, 2);
  const value = 'rm-a1';
  const reply = await postJson<ProfileRes>(locationConsumerUrl, '/profile/request', { value });
  ensure(reply.value === `profile:${value}`, 'RM-A1 reply value mismatch.');
  ensure(reply.providerRid === 'api-a' || reply.providerRid === 'api-b', 'RM-A1 provider rid mismatch.');

  const topology = await getJson<Array<{ channelName: string; serviceRole: number; routingId?: string; endpoint: string }>>(
    locationConsumerUrl,
    '/location/topology'
  );
  const profileProviders = topology.filter((entry) =>
    entry.channelName === 'profile'
    && entry.serviceRole === 3
    && (entry.routingId === 'api-a' || entry.routingId === 'api-b')
    && entry.endpoint.length > 0
  );
  ensure(
    profileProviders.length >= 2,
    `RM-A1 expected live peer rows for both profile providers: ${JSON.stringify(topology)}`
  );

  const providerEvidence = [
    ...await getJson<string[]>(providerAUrl, '/evidence'),
    ...await getJson<string[]>(providerBUrl, '/evidence')
  ];
  ensure(
    providerEvidence.filter((line) => line.includes('value=rm-a1')).length === 1
      && providerEvidence.some((line) => line.includes(`rid=${reply.providerRid}`) && line.includes('value=rm-a1')),
    'RM-A1 provider evidence did not contain the selected provider exactly once.'
  );
  console.log('scenario RM-A1 passed');
}

async function waitForReadyTargets(baseUrl: string, count: number): Promise<void> {
  const deadline = Date.now() + 10000;
  let last: {
    readonly channels: readonly {
      readonly channelName: string;
      readonly isReady: boolean;
      readonly readyTargetCount: number;
    }[];
  } | undefined;
  while (Date.now() < deadline) {
    last = await getJson<{
      readonly channels: readonly {
        readonly channelName: string;
        readonly isReady: boolean;
        readonly readyTargetCount: number;
      }[];
    }>(baseUrl, '/route/status');
    const channel = last.channels.find((candidate) => candidate.channelName === 'profile');
    if (channel?.isReady === true && channel.readyTargetCount === count) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`RM-A1 expected ${count} ready RouteMesh targets, last=${JSON.stringify(last)}`);
}
