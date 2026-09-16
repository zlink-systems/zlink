// PS-F3: Reserved liveness topic을 Application publish에서 거부한다 시나리오를 검증한다.
// Exact topic과 그 prefix를 공유하는 확장 topic을 모두 거부한다.
import { randomUUID } from 'node:crypto';
import { getJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';

const reservedLivenessTopic = '\x01ZLF1';

export async function runPsF3(publisher: string, subscribers: readonly string[]): Promise<void> {
  await expectRejected(publisher, subscribers, reservedLivenessTopic, 1, 'exact');
  await expectRejected(
    publisher,
    subscribers,
    `${reservedLivenessTopic}.application`,
    2,
    'extended'
  );
  console.log('scenario PS-F3 passed');
}

async function expectRejected(
  publisher: string,
  subscribers: readonly string[],
  topic: string,
  sequence: number,
  label: string
): Promise<void> {
  const runId = randomUUID().replaceAll('-', '');
  let rejection: unknown;
  try {
    await publishEvent(publisher, topic, runId, sequence, 'must-not-send');
  } catch (error) {
    rejection = error;
  }
  ensure(rejection instanceof Error, `PS-F3 expected the ${label} reserved topic to be rejected.`);
  const evidence = await Promise.all(
    subscribers.map((subscriber) => getJson<readonly string[]>(subscriber, '/evidence'))
  );
  ensure(
    evidence.every((lines) => lines.every((line) => !line.includes(`run=${runId}`))),
    `PS-F3 expected the ${label} rejected topic not to reach any subscriber.`
  );
}
