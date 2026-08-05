// PS-D7A: 느린 fanout status observer를 격리한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { getJson, postStatus } from '../../../http-client';
import { PubSubNames } from '../../Shared/messages';
import { ensure, eventually, isConnectionFailure } from '../Support/scenario-assert';
import { ServerProcessLauncher } from '../Support/server-process-launcher';
import { waitForEvent } from '../Support/subscriber-observation';
import { hasReadyPublishers } from './ps-d3-publisher-set-convergence-scenario';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';

export async function runPsD7A(
  publisherA: string,
  publisherB: string | undefined,
  subscriber: string,
  processes: ServerProcessLauncher
): Promise<void> {
  ensure(publisherB !== undefined, 'PS-D7A requires a second publisher HTTP endpoint.');
  await eventually(
    async () => hasReadyPublishers(subscriber, ['pub-a']),
    'PS-D7A expected publisher A to be ready before starting observers.',
    20_000
  );
  await postStatus(`${subscriber}/observer/fanout/slow/start`);
  await postStatus(`${subscriber}/observer/fanout/normal/start`);

  const publisherBProcess = processes.startPublisherNamed(
    'pub-b',
    publisherB,
    'tcp://127.0.0.1:0',
    'pub-b'
  );
  try {
    await publisherBProcess.waitReady();
    await eventually(
      async () => evidenceContains(subscriber, 'fanout-observer-slow', '|ready=2')
        && evidenceContains(subscriber, 'fanout-observer-normal', '|ready=2'),
      'PS-D7A expected both observers to receive the publisher-add status.',
      10_000
    );

    const eventRun = randomUUID().replaceAll('-', '');
    await publishEvent(publisherB, PubSubNames.mainTopic, eventRun, 1, 'observer-isolation');
    await waitForEvent(subscriber, eventRun, 1, 'observer-isolation', 10_000);

    await postStatus(`${publisherA}/shutdown`);
    await eventually(
      async () => {
        try {
          return (await getJson<{ readonly status?: string }>(publisherA, '/health')) === undefined;
        } catch (error) {
          return isConnectionFailure(error);
        }
      },
      'PS-D7A expected publisher A to stop after orderly shutdown.',
      10_000
    );
    await eventually(
      async () => hasReadyPublishers(subscriber, ['pub-b'])
        && evidenceContains(subscriber, 'fanout-observer-normal', '|ready=1'),
      'PS-D7A expected the normal observer to receive the current one-publisher status while the slow observer was blocked.',
      10_000
    );

    await postStatus(`${subscriber}/observer/fanout/slow/cancel`);
    const afterCancelRun = randomUUID().replaceAll('-', '');
    await publishEvent(publisherB, PubSubNames.mainTopic, afterCancelRun, 2, 'observer-after-cancel');
    await waitForEvent(subscriber, afterCancelRun, 2, 'observer-after-cancel', 10_000);
    ensure(
      await evidenceContains(subscriber, 'fanout-observer-normal', '|ready=1'),
      'PS-D7A expected cancelling the slow observer not to terminate the normal observer.'
    );
    console.log('scenario PS-D7A passed');
  } finally {
    await publisherBProcess.stop();
  }
}

async function evidenceContains(subscriber: string, observer: string, suffix: string): Promise<boolean> {
  const evidence = await getJson<readonly string[]>(subscriber, '/evidence').catch(() => []);
  return evidence.some((line) => line.includes(observer) && line.includes(suffix));
}
