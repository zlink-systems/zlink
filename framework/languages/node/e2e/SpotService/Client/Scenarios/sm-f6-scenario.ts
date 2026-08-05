// SM-F6: Cross-node Spot call과 Actor Join을 같은 RouteMesh에서 처리한다 시나리오를 검증한다.
import type {
  EvidenceWaitReq,
  CreateSpotReq,
  CreateSpotRes,
  SpotOnlyJoinReq,
  SpotOnlyJoinRes,
  SpotOnlyMeshReq,
  SpotOnlyMeshRes
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmF6(options: ClientOptions): Promise<void> {
  const sourceSpotId = `spot-sm-f6-source-${Date.now()}`;
  const targetSpotId = `spot-sm-f6-target-${Date.now()}`;
  const actorId = `actor-sm-f6-${Date.now()}`;
  const marker = `sm-f6-${Date.now()}`;

  await Promise.all([
    postJson(options.multiAUrl, '/placement/weight', { weight: 0 }),
    postJson(options.multiBUrl, '/placement/weight', { weight: 100 })
  ]);
  const created = await postJson<CreateSpotRes>(options.multiBUrl, '/spot/create-user-local', {
    spotId: targetSpotId
  } satisfies CreateSpotReq);
  ensure(created.nodeRid !== undefined, 'SM-F6 target Spot owner was not returned.');
  const targetUrl = created.nodeRid === 'multi-node-a' ? options.multiAUrl : options.multiBUrl;
  const sourceUrl = created.nodeRid === 'multi-node-a' ? options.multiBUrl : options.multiAUrl;
  await Promise.all([
    postJson(targetUrl, '/placement/weight', { weight: 0 }),
    postJson(sourceUrl, '/placement/weight', { weight: 100 })
  ]);

  const mesh = await postJson<SpotOnlyMeshRes>(sourceUrl, '/spot/spot-only/request-send', {
    sourceSpotId,
    targetSpotId,
    marker
  } satisfies SpotOnlyMeshReq);
  ensure(mesh.targetSpotId === targetSpotId, 'SM-F6 request target mismatch.');
  ensure(mesh.targetValue === 7, 'SM-F6 target request value mismatch.');

  const join = await postJson<SpotOnlyJoinRes>(sourceUrl, '/actor/spot-only-join', {
    targetSpotId,
    actorId,
    marker
  } satisfies SpotOnlyJoinReq);
  ensure(join.accepted, 'SM-F6 spot-only actor join was rejected.');
  ensure(join.actorId === actorId, 'SM-F6 actor join id mismatch.');

  const expected = [
    `spot-state-request|rid=${created.nodeRid}|spot=${targetSpotId}|value=7`,
    `spot-state-command|rid=${created.nodeRid}|spot=${targetSpotId}|marker=sm-f6-send-${marker}`,
    `spot-actor-joined|rid=${created.nodeRid}|spot=${targetSpotId}|actor=${actorId}`
  ];
  const evidence = await postJson<string[]>(targetUrl, '/evidence/wait', {
    containsAll: expected,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    evidence.some((line) => line.includes(`spot-actor-joined|rid=${created.nodeRid}|spot=${targetSpotId}|actor=${actorId}`)),
    'SM-F6 remote actor join evidence did not appear on the selected target node.'
  );

  console.log('scenario SM-F6 passed');
}
