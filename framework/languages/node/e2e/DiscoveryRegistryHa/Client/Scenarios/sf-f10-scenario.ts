// SF-F10: 많은 accepted requests와 relocation completion을 함께 처리한다 시나리오를 검증한다.
import { getJson, postJson } from '../../../http-client';
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';
import {
  objectLocation,
  relocate,
  setPlacementWeight,
  setScenarioGate,
  waitFor
} from '../Support/relocation-fixture';

interface ObjectReply {
  readonly spotId: string;
  readonly operationId: string;
  readonly payload: string;
  readonly providerRid: string;
}

export async function runSFF10(options: ClientOptions): Promise<void> {
  ensure(options.providerBUrl !== undefined, 'SF-F10 provider B URL is required.');
  const spotId = 'sf-f10-instance';
  const operationIds = Array.from({ length: 32 }, (_, index) => `accepted-${index}`);
  await setScenarioGate(options.providerAUrl, 'request', true);
  const requests = operationIds.map((operationId) => request(
    options,
    spotId,
    operationId,
    `payload-${operationId}`
  ));
  console.log('scenario-control SF-F10 start-provider-b');
  await waitFor(async () => {
    try {
      await getJson(options.providerBUrl!, '/health');
      return true;
    } catch {
      return false;
    }
  }, 'SF-F10 provider B readiness');
  await setPlacementWeight(options.providerAUrl, 0);
  const relocation = relocate(options.providerAUrl, 60_000);
  const replies = await Promise.all(requests);
  const result = await relocation;

  ensure(result.outcome === 0, 'SF-F10 relocation did not complete.');
  ensure(new Set(replies.map((reply) => reply.operationId)).size === operationIds.length,
    'SF-F10 returned duplicate or missing request terminals.');
  for (const reply of replies) {
    ensure(reply.payload === `payload-${reply.operationId}`,
      `SF-F10 payload changed for '${reply.operationId}'.`);
  }
  await waitFor(
    async () => (await objectLocation(options, 'spot', spotId)).ownerNodeRid === 'api-b',
    'SF-F10 target ownership'
  );
  const followUp = await request(options, spotId, 'follow-up', 'payload-follow-up');
  ensure(followUp.providerRid === 'api-b', 'SF-F10 follow-up was not handled by the current target.');
  ensure(followUp.operationId === 'follow-up' && followUp.payload === 'payload-follow-up',
    'SF-F10 follow-up terminal changed identity or payload.');
  console.log('scenario SF-F10 passed');
}

async function request(
  options: ClientOptions,
  spotId: string,
  operationId: string,
  payload: string
): Promise<ObjectReply> {
  return await postJson<ObjectReply>(options.consumerUrl, '/object/request', {
    spotId,
    operationId,
    payload
  });
}
