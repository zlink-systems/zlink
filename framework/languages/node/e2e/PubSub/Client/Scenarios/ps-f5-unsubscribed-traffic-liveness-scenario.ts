// PS-F5: 구독하지 않은 traffic 중에도 liveness를 유지한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { getJson } from '../../../http-client';
import { PubSubNames } from '../../Shared/messages';
import { delay, ensure, eventually } from '../Support/scenario-assert';
import { waitForEvent } from '../Support/subscriber-observation';
import { hasReadyPublishers } from './ps-d3-publisher-set-convergence-scenario';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';

export async function runPsF5(
  publisher: string,
  subscriber: string
): Promise<void> {
  await eventually(
    async () => hasReadyPublishers(subscriber, ['pub-f5']),
    'PS-F5 expected the automatic subscriber to mark the publisher ready.',
    20_000
  );

  const runId = randomUUID().replaceAll('-', '');
  const trafficDeadline = Date.now() + 16_000;
  let sequence = 1;
  while (Date.now() < trafficDeadline) {
    await publishEvent(
      publisher,
      PubSubNames.otherTopic,
      runId,
      sequence,
      'unsubscribed-traffic'
    );
    sequence += 1;
    await delay(1_000);
  }

  ensure(
    await hasReadyPublishers(subscriber, ['pub-f5']),
    'PS-F5 expected liveness to keep the publisher ready during unhandled traffic.'
  );
  const evidence = await getJson<readonly string[]>(subscriber, '/evidence');
  ensure(
    evidence.every((line) => !line.includes(`event|`) || !line.includes(`run=${runId}`)),
    'PS-F5 expected the unhandled traffic not to reach the application event evidence.'
  );

  const acceptedRun = randomUUID().replaceAll('-', '');
  await publishEvent(publisher, PubSubNames.mainTopic, acceptedRun, 1, 'after-unsubscribed-traffic');
  await waitForEvent(subscriber, acceptedRun, 1, 'after-unsubscribed-traffic', 10_000);
  ensure(
    await hasReadyPublishers(subscriber, ['pub-f5']),
    'PS-F5 expected the publisher to remain ready after the accepted event.'
  );
  console.log('scenario PS-F5 passed');
}
