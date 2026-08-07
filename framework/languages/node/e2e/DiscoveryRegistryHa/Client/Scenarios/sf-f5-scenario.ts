// SF-F5: Creating owner crash 뒤 pending과 follow-up request가 각각 한 terminal을 얻는지 검증한다.
import { postJsonWithin } from '../../../http-client';
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';
import { setScenarioGate } from '../Support/relocation-fixture';

interface ObjectReply {
  readonly spotId: string;
  readonly operationId: string;
  readonly payload: string;
  readonly providerRid: string;
}

export async function runSFF5(options: ClientOptions): Promise<void> {
  const spotId = 'sf-f5-instance';
  await setScenarioGate(options.providerAUrl, 'initialize', true);
  let pendingTerminalCount = 0;
  const pending = request(options, spotId, 'pending', 'pending-payload')
    .then((value) => {
      pendingTerminalCount += 1;
      return { kind: 'success' as const, value };
    }, (error: unknown) => {
      pendingTerminalCount += 1;
      return { kind: 'failure' as const, error };
    });
  console.log('scenario-control SF-F5 pending-request-started');
  const first = await pending;
  ensure(pendingTerminalCount === 1, 'SF-F5 pending request produced more than one terminal.');
  if (first.kind === 'success') {
    ensure(first.value.operationId === 'pending', 'SF-F5 pending success belonged to another operation.');
  }

  const followUp = await request(options, spotId, 'follow-up', 'follow-up-payload')
    .then((value) => ({ kind: 'success' as const, value }), (error: unknown) => ({ kind: 'failure' as const, error }));
  if (followUp.kind === 'success') {
    ensure(followUp.value.operationId === 'follow-up', 'SF-F5 follow-up terminal belonged to another operation.');
    ensure(followUp.value.payload === 'follow-up-payload', 'SF-F5 follow-up payload changed during recovery.');
    ensure(followUp.value.providerRid === 'api-b', 'SF-F5 recovered request was not handled by api-b.');
    console.log('scenario SF-F5 follow-up recovery=success');
  } else {
    const message = followUp.error instanceof Error ? followUp.error.message : String(followUp.error);
    ensure(message.length > 0, 'SF-F5 follow-up failure did not produce a terminal error.');
    console.log(`scenario SF-F5 follow-up recovery=failure message=${message}`);
  }
  console.log('scenario SF-F5 passed');
}

async function request(
  options: ClientOptions,
  spotId: string,
  operationId: string,
  payload: string
): Promise<ObjectReply> {
  return await postJsonWithin<ObjectReply>(options.consumerUrl, '/object/request', {
    spotId,
    operationId,
    payload
  }, 15_000);
}
