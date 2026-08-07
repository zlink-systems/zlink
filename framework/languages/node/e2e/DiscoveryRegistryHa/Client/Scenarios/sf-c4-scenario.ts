// SF-C4: 여러 service role을 가진 host를 한 lifecycle로 정리한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';
import type { ClientServerRes, ProfileRes, SecondaryRes } from '../../Shared/messages';

export async function runSFC4(options: ClientOptions): Promise<void> {
  const marker = `sf-c4-${Date.now().toString(36)}`;
  const profile = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', {
    value: `${marker}-profile`, marker: 'SF-C4'
  });
  const secondary = await postJson<SecondaryRes>(options.consumerUrl, '/c4/secondary', {
    value: `${marker}-secondary`, marker: 'SF-C4'
  });
  const clientServer = await postJson<ClientServerRes>(options.consumerUrl, '/c4/client-server', {
    value: `${marker}-client-server`, marker: 'SF-C4'
  });
  await postJson(options.providerAUrl, '/c4/fanout', {
    value: `${marker}-fanout`, marker: 'SF-C4'
  });
  ensure(profile.providerRid.length > 0, 'SF-C4 profile marker did not identify a provider.');
  ensure(secondary.providerRid.length > 0, 'SF-C4 secondary marker did not identify a provider.');
  ensure(clientServer.providerRid.length > 0, 'SF-C4 ClientServer marker did not identify a provider.');

  const providerEvidence = await getJson<readonly string[]>(options.providerAUrl, '/evidence');
  const consumerEvidence = await waitForFanoutEvidence(options.consumerUrl, marker);
  for (const value of ['profile', 'secondary', 'client-server']) {
    ensure(
      providerEvidence.filter((entry) => entry.includes(`value=${marker}-${value}`)).length === 1,
      `SF-C4 replacement provider did not process ${value} exactly once.`
    );
  }
  ensure(
    consumerEvidence.filter((entry) => entry.includes(`value=${marker}-fanout`)).length === 1,
    'SF-C4 replacement fanout subscriber did not process the marker exactly once.'
  );
  console.log('scenario SF-C4 passed');
}

async function waitForFanoutEvidence(
  consumerUrl: string,
  marker: string
): Promise<readonly string[]> {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    const evidence = await getJson<readonly string[]>(consumerUrl, '/evidence');
    if (evidence.filter((entry) => entry.includes(`value=${marker}-fanout`)).length > 0) {
      return evidence;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  return await getJson<readonly string[]>(consumerUrl, '/evidence');
}
