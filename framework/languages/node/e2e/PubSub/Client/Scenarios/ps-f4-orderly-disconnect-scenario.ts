// PS-F4: Orderly disconnect를 peer deadline 전에 반영한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { getStatus, postStatus } from '../../../http-client';
import { PubSubNames } from '../../Shared/messages';
import { ensure, eventually, isConnectionFailure } from '../Support/scenario-assert';
import { ServerProcessLauncher } from '../Support/server-process-launcher';
import { waitForEvent } from '../Support/subscriber-observation';
import { hasReadyPublishers } from './ps-d3-publisher-set-convergence-scenario';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';

export async function runPsF4(
  publisherA: string,
  publisherB: string | undefined,
  subscriber: string,
  processes: ServerProcessLauncher
): Promise<void> {
  ensure(publisherB !== undefined, 'PS-F4 requires a second publisher HTTP endpoint.');
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
      'PS-F4 expected both publishers to be ready before orderly disconnect.',
      20_000
    );

    const shutdownStartedAt = Date.now();
    await postStatus(`${publisherA}/shutdown`);
    await eventually(
      async () => {
        try {
          return (await getStatus(`${publisherA}/health`)) !== 200;
        } catch (error) {
          return isConnectionFailure(error);
        }
      },
      'PS-F4 expected publisher A to terminate after orderly shutdown.',
      10_000
    );
    await eventually(
      async () => hasReadyPublishers(subscriber, ['pub-b']),
      'PS-F4 expected orderly disconnect to remove publisher A before peer deadline.',
      10_000
    );
    ensure(
      Date.now() - shutdownStartedAt < 15_000,
      'PS-F4 removed the orderly publisher only after the 15-second peer deadline.'
    );

    const runId = randomUUID().replaceAll('-', '');
    await publishEvent(publisherB, PubSubNames.mainTopic, runId, 1, 'publisher-b-after-orderly-disconnect');
    await waitForEvent(subscriber, runId, 1, 'publisher-b-after-orderly-disconnect', 10_000);
    console.log('scenario PS-F4 passed');
  } finally {
    await publisherProcess.stop();
  }
}
