// PS-C1: 등록되지 않은 packet name을 drop한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { PacketNames, PubSubNames } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { publishEvent, waitForAll } from './ps-a1-fanout-basic-delivery-scenario';

export async function runPsC1(publisher: string, subscribers: readonly string[]): Promise<void> {
  const runId = randomUUID().replaceAll('-', '');
  await postJson(publisher, '/publish/missing', {
    topic: PubSubNames.mainTopic,
    runId,
    sequence: 1,
    value: 'missing'
  });

  await waitForAll(subscribers, {
    containsAllLineGroups: [['dispatch-error|', `packet=${PacketNames.missingEventMsg}`, `topic=${PubSubNames.mainTopic}`]],
    timeoutMilliseconds: 10_000
  });

  await publishEvent(publisher, PubSubNames.mainTopic, runId, 2, 'after-missing');
  await waitForAll(subscribers, {
    containsAll: ['event|', `run=${runId}`, `topic=${PubSubNames.mainTopic}`],
    timeoutMilliseconds: 10_000
  });
  console.log('scenario PS-C1 passed');
}
