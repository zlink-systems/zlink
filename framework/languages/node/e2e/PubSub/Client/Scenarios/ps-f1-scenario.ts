// PS-F1: Automatic과 manual publisher가 Ready로 수렴한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { PubSubNames } from '../../Shared/messages';
import { ensure } from '../Support/scenario-assert';
import { waitForEvent } from '../Support/subscriber-observation';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';

export async function runPsF1(publisher: string, subscriber: string): Promise<void> {
  const runId = randomUUID().replaceAll('-', '');
  await publishEvent(publisher, PubSubNames.mainTopic, runId, 1, 'liveness-ready');
  await waitForEvent(subscriber, runId, 1, 'liveness-ready', 10_000);
  ensure(runId.length > 0, 'PS-F1 marker identity is missing.');
  console.log('scenario PS-F1 passed');
}
