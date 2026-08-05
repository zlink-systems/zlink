// PS-A1: 준비된 subscriber가 같은 event를 받는다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { PubSubNames, type EvidenceWaitReq } from '../../Shared/messages';
import { commonContiguousSequence } from '../Support/evidence';
import { postJson, postJsonWithin, postStatus } from '../../../http-client';
import { delay, ensure } from '../Support/scenario-assert';
import { waitForEvent } from '../Support/subscriber-observation';

export async function runPsA1(publisher: string, subscribers: readonly string[]): Promise<void> {
  const runId = randomUUID().replaceAll('-', '');
  for (let i = 1; i <= 120; i += 1) {
    await publishEvent(publisher, PubSubNames.mainTopic, runId, i, `warmup-${i}`);
    if (i % 10 === 0) {
      await delay(25);
    }
  }
  await waitForAll(subscribers, { containsAll: ['event|', `run=${runId}`, `topic=${PubSubNames.mainTopic}`], timeoutMilliseconds: 10_000 });

  const measureStart = 1000;
  const measureCount = 12;
  for (let i = measureStart; i < measureStart + measureCount; i += 1) {
    await publishEvent(publisher, PubSubNames.mainTopic, runId, i, `measure-${i}`);
  }
  const snapshots = await waitForAll(subscribers, {
    containsAll: ['event|', `run=${runId}`, `topic=${PubSubNames.mainTopic}`],
    containsAllLineGroups: [0, 1, 2].map((offset) => [
      `seq=${measureStart + offset}|`,
      `value=measure-${measureStart + offset}`
    ]),
    timeoutMilliseconds: 10_000
  });
  ensure(
    commonContiguousSequence(
      snapshots,
      runId,
      PubSubNames.mainTopic,
      measureStart,
      measureStart + measureCount - 1,
      'measure-'
    ).length >= 3,
    'PS-A1 expected common contiguous sequence on all subscribers.'
  );
  console.log('scenario PS-A1 passed');
}

export async function publishEvent(publisher: string, topic: string, runId: string, sequence: number, value: string): Promise<void> {
  await postJson(publisher, '/publish/event', { topic, runId, sequence, value });
}

export async function publishUntilDelivered(
  publisher: string,
  subscriber: string,
  topic: string,
  sequence: number,
  value: string,
  timeoutMilliseconds = 20_000
): Promise<string> {
  const deadline = Date.now() + timeoutMilliseconds;
  while (Date.now() < deadline) {
    const runId = randomUUID().replaceAll('-', '');
    await publishEvent(publisher, topic, runId, sequence, value);
    try {
      await waitForEvent(subscriber, runId, sequence, value, 750);
      return runId;
    } catch {
      await delay(100);
    }
  }
  throw new Error(`Timed out waiting for fanout delivery of value '${value}'.`);
}

export async function publishUntilAllDelivered(
  publisher: string,
  subscribers: readonly string[],
  topic: string,
  sequence: number,
  value: string,
  timeoutMilliseconds = 20_000
): Promise<string> {
  const deadline = Date.now() + timeoutMilliseconds;
  while (Date.now() < deadline) {
    const runId = randomUUID().replaceAll('-', '');
    await Promise.all(subscribers.map((subscriber) => postStatus(`${subscriber}/evidence/clear`)));
    await publishEvent(publisher, topic, runId, sequence, value);
    try {
      await waitForAll(subscribers, {
        containsAll: ['event|', `run=${runId}`, `topic=${topic}`],
        timeoutMilliseconds: 750
      });
      return runId;
    } catch {
      await delay(100);
    }
  }
  throw new Error(`Timed out waiting for all fanout subscribers to receive value '${value}'.`);
}

export async function waitForAll(subscribers: readonly string[], request: EvidenceWaitReq): Promise<readonly (readonly string[])[]> {
  const timeoutMilliseconds = request.timeoutMilliseconds ?? 10_000;
  return await Promise.all(subscribers.map((subscriber) => postJsonWithin<readonly string[]>(
    subscriber,
    '/evidence/wait',
    request,
    timeoutMilliseconds + 1_000
  )));
}
