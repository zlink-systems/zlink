// PS-F2: Publisher 하나의 수신 단절을 다른 publisher와 분리한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { PubSubNames } from '../../Shared/messages';
import { ensure, eventually } from '../Support/scenario-assert';
import { NetworkFaultProxy } from '../Support/network-fault-proxy';
import { ServerProcessLauncher } from '../Support/server-process-launcher';
import { waitForEvent } from '../Support/subscriber-observation';
import { hasReadyPublishers } from './ps-d3-publisher-set-convergence-scenario';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';

export async function runPsF2(
  publisherAUrl: string,
  publisherBUrl: string | undefined,
  subscriberUrl: string,
  publisherProxyPort: number | undefined,
  processes: ServerProcessLauncher
): Promise<void> {
  ensure(publisherBUrl !== undefined, 'PS-F2 requires a second publisher HTTP endpoint.');
  ensure(publisherProxyPort !== undefined, 'PS-F2 requires a dedicated publisher fault-proxy port.');
  const publisherBFault = await NetworkFaultProxy.start(
    `tcp://127.0.0.2:${publisherProxyPort}`,
    false,
    publisherProxyPort
  );
  const publisherA = processes.startPublisherNamed(
    'pub-a',
    publisherAUrl,
    'tcp://127.0.0.1:0',
    'pub-a'
  );
  const publisherB = processes.startPublisherNamed(
    'pub-b',
    publisherBUrl,
    `tcp://127.0.0.2:${publisherProxyPort}`,
    'pub-b',
    '127.0.0.1'
  );
  const subscriber = processes.startSubscriber(
    'sub-f2',
    subscriberUrl,
    'sub-f2.evidence.log'
  );
  try {
    await Promise.all([publisherA.waitReady(), publisherB.waitReady(), subscriber.waitReady()]);
    await eventually(
      async () => hasReadyPublishers(subscriberUrl, ['pub-a', 'pub-b']),
      'PS-F2 expected both publishers to be ready before isolating publisher B.',
      20_000
    );
    publisherBFault.block();
    await eventually(
      async () => hasReadyPublishers(subscriberUrl, ['pub-a']),
      'PS-F2 expected only publisher B to leave Ready after its inbound path was blocked.',
      20_000
    );

    const aRun = randomUUID().replaceAll('-', '');
    await publishEvent(publisherAUrl, PubSubNames.mainTopic, aRun, 1, 'publisher-a-during-b-failure');
    await waitForEvent(subscriberUrl, aRun, 1, 'publisher-a-during-b-failure', 10_000);
    ensure(!publisherA.hasExited && !publisherB.hasExited && !subscriber.hasExited,
      'PS-F2 expected the host and both publisher processes to remain available.');

    publisherBFault.unblock();
    await eventually(
      async () => hasReadyPublishers(subscriberUrl, ['pub-a', 'pub-b']),
      'PS-F2 expected publisher B to become ready after its inbound path recovered.',
      20_000
    );
    const bRun = randomUUID().replaceAll('-', '');
    await publishEvent(publisherBUrl, PubSubNames.mainTopic, bRun, 2, 'publisher-b-after-recovery');
    await waitForEvent(subscriberUrl, bRun, 2, 'publisher-b-after-recovery', 10_000);
    console.log('scenario PS-F2 passed');
  } finally {
    await subscriber.stop();
    await publisherA.stop();
    await publisherB.stop();
    await publisherBFault.close();
  }
}
