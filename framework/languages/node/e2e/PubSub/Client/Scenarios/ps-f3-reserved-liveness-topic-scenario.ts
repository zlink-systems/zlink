// PS-F3: Reserved liveness topic을 Application publish에서 거부한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { getJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';
import { publishEvent, waitForAll } from './ps-a1-fanout-basic-delivery-scenario';

const reservedLivenessTopic = '\x01ZLF1';

export async function runPsF3(publisher: string, subscribers: readonly string[]): Promise<void> {
  const rejectedRunId = randomUUID().replaceAll('-', '');
  let rejection: unknown;
  try {
    await publishEvent(publisher, reservedLivenessTopic, rejectedRunId, 1, 'must-not-send');
  } catch (error) {
    rejection = error;
  }
  ensure(rejection instanceof Error, 'PS-F3 expected the exact reserved liveness topic to be rejected.');
  const rejectedEvidence = await Promise.all(
    subscribers.map((subscriber) => getJson<readonly string[]>(subscriber, '/evidence'))
  );
  ensure(
    rejectedEvidence.every((lines) => lines.every((line) => !line.includes(`run=${rejectedRunId}`))),
    'PS-F3 expected the rejected topic not to reach any subscriber.'
  );

  const extendedTopic = `${reservedLivenessTopic}.application`;
  const acceptedRunId = randomUUID().replaceAll('-', '');
  await publishEvent(publisher, extendedTopic, acceptedRunId, 2, 'prefix-is-allowed');
  const evidence = await waitForAll(subscribers, {
    containsAllLineGroups: [[
      'ignored|',
      `run=${acceptedRunId}`,
      `topic=${extendedTopic}`,
      'value=prefix-is-allowed'
    ]],
    timeoutMilliseconds: 10_000
  });
  ensure(
    evidence.every((lines) => lines.filter((line) => line.includes(`run=${acceptedRunId}`)).length === 1),
    'PS-F3 expected the longer topic to reach each typed handler exactly once.'
  );
  console.log('scenario PS-F3 passed');
}
