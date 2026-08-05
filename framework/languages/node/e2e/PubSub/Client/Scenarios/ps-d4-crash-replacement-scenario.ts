// PS-D4: Crash한 publisher를 replacement로 바꾼다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { PubSubNames } from '../../Shared/messages';
import { delay, ensure, eventually } from '../Support/scenario-assert';
import { ServerProcessLauncher, type DynamicProcess } from '../Support/server-process-launcher';
import { waitForEvent } from '../Support/subscriber-observation';
import { hasReadyPublishers } from './ps-d3-publisher-set-convergence-scenario';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';

export async function runPsD4(
  publisherUrl: string,
  replacementUrl: string | undefined,
  subscriberUrl: string,
  processes: ServerProcessLauncher
): Promise<void> {
  ensure(replacementUrl !== undefined, 'PS-D4 requires a replacement publisher HTTP endpoint.');
  const subscriber = processes.startSubscriber(
    'sub-d4',
    subscriberUrl,
    'sub-d4.evidence.log'
  );
  const publisher = processes.startPublisherNamed(
    'pub-a',
    publisherUrl,
    'tcp://127.0.0.1:0',
    'pub-a'
  );
  let replacement: DynamicProcess | undefined;
  try {
    await Promise.all([subscriber.waitReady(), publisher.waitReady()]);
    await eventually(
      async () => hasReadyPublishers(subscriberUrl, ['pub-a']),
      'PS-D4 expected the initial publisher to be ready.',
      20_000
    );
    const baselineRun = randomUUID().replaceAll('-', '');
    await publishEvent(publisherUrl, PubSubNames.mainTopic, baselineRun, 1, 'publisher-before-crash');
    await waitForEvent(subscriberUrl, baselineRun, 1, 'publisher-before-crash', 10_000);

    await publisher.kill();
    await eventually(
      async () => hasReadyPublishers(subscriberUrl, []),
      'PS-D4 expected the crashed publisher to leave the ready set.',
      20_000
    );
    // The PubSub E2E location policy uses a 5-second owner lease. A crash
    // leaves that lease in the Store until its TTL expires, so replacement
    // admission must wait for the explicit lease-expiry boundary.
    await delay(6_000);

    replacement = processes.startPublisherNamed(
      'pub-a-replacement',
      replacementUrl,
      'tcp://127.0.0.1:0',
      'pub-a'
    );
    await replacement.waitReady();
    await eventually(
      async () => hasReadyPublishers(subscriberUrl, ['pub-a']),
      'PS-D4 expected the replacement publisher to become ready.',
      20_000
    );
    const replacementRun = randomUUID().replaceAll('-', '');
    await publishEvent(replacementUrl, PubSubNames.mainTopic, replacementRun, 2, 'publisher-after-replacement');
    await waitForEvent(subscriberUrl, replacementRun, 2, 'publisher-after-replacement', 10_000);
    console.log('scenario PS-D4 passed');
  } finally {
    if (replacement !== undefined) await replacement.stop();
    if (!publisher.hasExited) await publisher.kill();
    await subscriber.stop();
  }
}
