// PS-A2: Packet name으로 typed handler를 선택한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { PubSubNames } from '../../Shared/messages';
import { isEvent } from '../Support/evidence';
import { ensure } from '../Support/scenario-assert';
import { publishEvent, waitForAll } from './ps-a1-fanout-basic-delivery-scenario';

export async function runPsA2(publisher: string, subscribers: readonly string[]): Promise<void> {
  const runId = randomUUID().replaceAll('-', '');
  await publishEvent(publisher, PubSubNames.mainTopic, runId, 1, 'accepted');
  await publishEvent(publisher, PubSubNames.otherTopic, runId, 2, 'ignored');

  const snapshots = await waitForAll(subscribers, {
    containsAll: ['event|', `run=${runId}`, `topic=${PubSubNames.mainTopic}`],
    containsAllLineGroups: [['ignored|', `run=${runId}`, `topic=${PubSubNames.otherTopic}`]],
    timeoutMilliseconds: 10_000
  });

  ensure(snapshots.every((lines) => lines.some((line) => isEvent(line, runId, PubSubNames.mainTopic))), 'PS-A2 accepted topic was not recorded.');
  ensure(
    snapshots.every((lines) => lines.every((line) => !line.includes('event|') || !line.includes(`run=${runId}`) || !line.includes(`topic=${PubSubNames.otherTopic}`))),
    'PS-A2 non-interest topic was recorded as accepted.'
  );
  console.log('scenario PS-A2 passed');
}
