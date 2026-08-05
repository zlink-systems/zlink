// PS-B1: 느린 subscriber가 다른 subscriber를 막지 않는다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { PubSubNames } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';

export async function runPsB1(publisher: string, fastSubscribers: readonly string[], slowSubscriber: string): Promise<void> {
  const runId = randomUUID().replaceAll('-', '');
  for (let i = 1; i <= 8; i += 1) {
    await publishEvent(publisher, PubSubNames.mainTopic, runId, i, i === 1 ? 'slow-first' : `fast-after-slow-${i}`);
  }

  await Promise.all(fastSubscribers.map((subscriber) => postJson<readonly string[]>(subscriber, '/evidence/wait', {
    containsAllLineGroups: [['event|', `run=${runId}`, `topic=${PubSubNames.mainTopic}`, 'seq=8']],
    timeoutMilliseconds: 2000
  })));

  const slowEvidence = await postJson<readonly string[]>(slowSubscriber, '/evidence/wait', {
    containsAllLineGroups: [['delay-start|', `run=${runId}`]],
    timeoutMilliseconds: 10_000
  });
  ensure(
    slowEvidence.some((line) => line.includes('delay-start|') && line.includes(`run=${runId}`)),
    'PS-B1 expected slow subscriber delay evidence.'
  );
  console.log('scenario PS-B1 passed');
}
