// PS-D3: Publisher 추가와 정상 제거에 수렴한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { getJson, getStatus, postStatus } from '../../../http-client';
import { PubSubNames } from '../../Shared/messages';
import { ensure, eventually, isConnectionFailure } from '../Support/scenario-assert';
import { ServerProcessLauncher } from '../Support/server-process-launcher';
import { waitForEvent } from '../Support/subscriber-observation';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';

export interface FanoutStatusWire {
  readonly isReady: boolean;
  readonly readyPublisherCount: number;
  readonly publishers: readonly {
    readonly nodeRid: string;
    readonly state: number;
  }[];
}

export async function runPsD3(
  publisherA: string,
  publisherB: string | undefined,
  subscriber: string,
  processes: ServerProcessLauncher
): Promise<void> {
  ensure(publisherB !== undefined, 'PS-D3 requires a second publisher HTTP endpoint.');
  await eventually(
    async () => hasReadyPublishers(subscriber, ['pub-a']),
    'PS-D3 expected the initial publisher to be ready.',
    20_000
  );

  const baselineRun = randomUUID().replaceAll('-', '');
  await publishEvent(publisherA, PubSubNames.mainTopic, baselineRun, 1, 'publisher-a-baseline');
  await waitForEvent(subscriber, baselineRun, 1, 'publisher-a-baseline', 10_000);

  const publisherProcess = processes.startPublisherNamed(
    'pub-b',
    publisherB,
    'tcp://127.0.0.1:0',
    'pub-b'
  );
  try {
    await publisherProcess.waitReady();
    await eventually(
      async () => hasReadyPublishers(subscriber, ['pub-a', 'pub-b']),
      'PS-D3 expected the subscriber to converge to both ready publishers.',
      20_000
    );

    const aRun = randomUUID().replaceAll('-', '');
    const bRun = randomUUID().replaceAll('-', '');
    await publishEvent(publisherA, PubSubNames.mainTopic, aRun, 2, 'publisher-a-current');
    await publishEvent(publisherB, PubSubNames.mainTopic, bRun, 3, 'publisher-b-current');
    await Promise.all([
      waitForEvent(subscriber, aRun, 2, 'publisher-a-current', 10_000),
      waitForEvent(subscriber, bRun, 3, 'publisher-b-current', 10_000)
    ]);

    await postStatus(`${publisherA}/shutdown`);
    await eventually(
      async () => {
        try {
          return (await getStatus(`${publisherA}/health`)) !== 200;
        } catch (error) {
          return isConnectionFailure(error);
        }
      },
      'PS-D3 expected publisher A to terminate after orderly shutdown.',
      10_000
    );
    await eventually(
      async () => hasReadyPublishers(subscriber, ['pub-b']),
      'PS-D3 expected publisher A to be removed and publisher B to remain ready.',
      20_000
    );

    const afterRemovalRun = randomUUID().replaceAll('-', '');
    await publishEvent(publisherB, PubSubNames.mainTopic, afterRemovalRun, 4, 'publisher-b-after-removal');
    await waitForEvent(subscriber, afterRemovalRun, 4, 'publisher-b-after-removal', 10_000);
    ensure(
      await hasReadyPublishers(subscriber, ['pub-b']),
      'PS-D3 expected only publisher B in the final public fanout status.'
    );
    console.log('scenario PS-D3 passed');
  } finally {
    await publisherProcess.stop();
  }
}

export async function hasReadyPublishers(
  subscriber: string,
  expected: readonly string[]
): Promise<boolean> {
  const status = await getJson<FanoutStatusWire>(subscriber, '/status/fanout').catch(() => undefined);
  if (status === undefined || status.readyPublisherCount !== expected.length) return false;
  if (expected.length > 0 && status.isReady !== true) return false;
  const ready = status.publishers
    .filter((publisher) => publisher.state === 1)
    .map((publisher) => publisher.nodeRid)
    .sort();
  return ready.length === expected.length
    && JSON.stringify(ready) === JSON.stringify([...expected].sort());
}
