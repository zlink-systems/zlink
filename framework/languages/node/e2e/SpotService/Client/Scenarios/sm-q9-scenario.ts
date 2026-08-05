// SM-Q9: 두 노드의 local Spot route가 각 owner에서 독립적으로 처리되는지 검증한다.
import type {
  EvidenceWaitReq,
  MultiNodeCreateSpotRes,
  MultiNodeCreateSpotReq,
  MultiNodeStateRouteReq,
  StateRes
} from '../../Shared/messages';
import { SpotServiceNames } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmQ9(options: ClientOptions): Promise<void> {
  const spotA = `spot-sm-q9-a-${uniqueId()}`;
  const spotB = `spot-sm-q9-b-${uniqueId()}`;

  const createdA = await postJson<MultiNodeCreateSpotRes>(options.multiAUrl, '/spot/create-local', {
    spotId: spotA,
    delta: 0
  } satisfies MultiNodeCreateSpotReq);
  const firstA = await postJson<StateRes>(options.multiAUrl, '/spot/state/request', {
    spotId: spotA,
    delta: 11
  } satisfies MultiNodeStateRouteReq);
  const directA = await postJson<StateRes>(options.multiAUrl, '/spot/state/request', {
    spotId: spotA,
    delta: 0
  } satisfies MultiNodeStateRouteReq);
  const evidenceA = await postJson<string[]>(options.multiAUrl, '/evidence/wait', {
    containsAll: [`multi-state-request|node=${SpotServiceNames.multiSpotNodeA}|spot=${spotA}|value=11`],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);

  const createdB = await postJson<MultiNodeCreateSpotRes>(options.multiBUrl, '/spot/create-local', {
    spotId: spotB,
    delta: 0
  } satisfies MultiNodeCreateSpotReq);
  const firstB = await postJson<StateRes>(options.multiBUrl, '/spot/state/request', {
    spotId: spotB,
    delta: 17
  } satisfies MultiNodeStateRouteReq);
  const directB = await postJson<StateRes>(options.multiBUrl, '/spot/state/request', {
    spotId: spotB,
    delta: 0
  } satisfies MultiNodeStateRouteReq);
  const evidenceB = await postJson<string[]>(options.multiBUrl, '/evidence/wait', {
    containsAll: [`multi-state-request|node=${SpotServiceNames.multiSpotNodeB}|spot=${spotB}|value=17`],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);

  const expectedA = `multi-state-request|node=${SpotServiceNames.multiSpotNodeA}|spot=${spotA}|value=11`;
  const expectedB = `multi-state-request|node=${SpotServiceNames.multiSpotNodeB}|spot=${spotB}|value=17`;
  ensure(
    evidenceA.filter((line) => line === expectedA).length >= 2,
    'SM-Q9 node A did not process both route-to-spot requests.'
  );
  ensure(
    evidenceB.filter((line) => line === expectedB).length >= 2,
    'SM-Q9 node B did not process both route-to-spot requests.'
  );

  ensure(createdA.nodeRid === SpotServiceNames.multiSpotNodeA, 'SM-Q9 node A create reply node mismatch.');
  ensure(firstA.value === 11, 'SM-Q9 node A route-to-spot reply value mismatch.');
  ensure(directA.spotId === spotA, 'SM-Q9 node A direct spot reply target mismatch.');
  ensure(directA.nodeRid === SpotServiceNames.multiSpotNodeA, 'SM-Q9 node A direct spot reply node mismatch.');
  ensure(directA.value === 11, 'SM-Q9 node A direct spot reply value mismatch.');
  ensure(createdB.nodeRid === SpotServiceNames.multiSpotNodeB, 'SM-Q9 node B create reply node mismatch.');
  ensure(firstB.value === 17, 'SM-Q9 node B route-to-spot reply value mismatch.');
  ensure(directB.spotId === spotB, 'SM-Q9 node B direct spot reply target mismatch.');
  ensure(directB.nodeRid === SpotServiceNames.multiSpotNodeB, 'SM-Q9 node B direct spot reply node mismatch.');
  ensure(directB.value === 17, 'SM-Q9 node B direct spot reply value mismatch.');

  console.log('operation SpotService.sm-q9 passed');
}

function uniqueId(): string {
  return `${Date.now().toString(16)}-${Math.random().toString(16).slice(2)}`;
}
