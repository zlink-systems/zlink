// RM-C2: RID를 지정한 Node direct request 시나리오를 검증한다.
import type { RouteMissingRes, ScenarioRouteRes } from '../../Shared/messages';
import { getJson, postJson } from '../../../http-client';
import { ensure, uniqueMarker } from '../Support/scenario-assert';

const REQUEST_TARGET_NOT_FOUND = '0';

export async function runRmC2(providerAUrl: string, providerBUrl: string): Promise<void> {
  const marker = uniqueMarker('rm-c2');
  const reply = await postJson<ScenarioRouteRes>(providerAUrl, '/profile/route/request', { value: marker });
  ensure(reply.providerRid === 'api-b', 'RM-C2 targeted route request should reach api-b.');
  ensure(reply.value === `route:${marker}`, 'RM-C2 route reply value mismatch.');

  const afterB = await postJson<string[]>(providerBUrl, '/evidence/wait', { contains: marker });
  const afterA = await getJson<string[]>(providerAUrl, '/evidence');
  ensure(
    afterB.filter((line) => line.includes('route-request|rid=api-b') && line.includes(marker)).length === 1
      && afterA.filter((line) => line.includes('route-request|rid=api-a') && line.includes(marker)).length === 0,
    'RM-C2 targeted route evidence did not match api-b only.'
  );

  const missing = await postJson<RouteMissingRes>(providerAUrl, '/profile/route/missing', { value: 'missing' });
  ensure(
    missing.failed && missing.errorKind === REQUEST_TARGET_NOT_FOUND,
    `RM-C2 missing rid should report RequestTargetNotFound, got '${missing.errorKind}'.`
  );
  console.log('scenario RM-C2 passed');
}
