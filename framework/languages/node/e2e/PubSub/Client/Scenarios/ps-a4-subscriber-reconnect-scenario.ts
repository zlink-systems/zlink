// PS-A4: Subscriber reconnect 뒤 새 event를 받는다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { PubSubNames } from '../../Shared/messages';
import { getJson } from '../../../http-client';
import { NetworkFaultProxy } from '../Support/network-fault-proxy';
import { ensure } from '../Support/scenario-assert';
import {
  waitForEvent,
  waitForNoEvent
} from '../Support/subscriber-observation';
import { ServerProcessLauncher } from '../Support/server-process-launcher';
import { publishEvent, publishUntilDelivered } from './ps-a1-fanout-basic-delivery-scenario';

export async function runPsA4(
  publisher: string,
  reconnectSubscriberUrl: string,
  fastSubscribers: readonly string[],
  processes: ServerProcessLauncher,
  publisherEndpoint: string
): Promise<void> {
  const runId = randomUUID().replaceAll('-', '');
  const fault = await NetworkFaultProxy.start(publisherEndpoint, true);
  const subscriber = processes.startSubscriber(
    'sub-reconnect', reconnectSubscriberUrl, 'sub-reconnect.evidence.log', fault.endpoint
  );
  try {
    await subscriber.waitReady();
    fault.unblock();
    const baselineRun = await publishUntilDelivered(
      publisher, reconnectSubscriberUrl, PubSubNames.mainTopic, 1, 'before-disconnect'
    );
    await Promise.all([
      ...fastSubscribers.map((url) => waitForEvent(url, baselineRun, 1, 'before-disconnect'))
    ]);

    fault.block();
    const disconnectedRun = runId;
    await publishEvent(publisher, PubSubNames.mainTopic, disconnectedRun, 2, 'while-disconnected');
    await Promise.all(fastSubscribers.map((url) => waitForEvent(url, disconnectedRun, 2, 'while-disconnected')));
    await waitForNoEvent(reconnectSubscriberUrl, disconnectedRun, 2, 'while-disconnected');

    fault.unblock();
    const afterReconnectRun = await publishUntilDelivered(
      publisher, reconnectSubscriberUrl, PubSubNames.mainTopic, 3, 'after-reconnect'
    );
    await Promise.all([
      ...fastSubscribers.map((url) => waitForEvent(url, afterReconnectRun, 3, 'after-reconnect'))
    ]);
    const evidence = await getJson<readonly string[]>(reconnectSubscriberUrl, '/evidence');
    ensure(
      !subscriber.hasExited
        && evidence.every((line) => !line.includes(`run=${disconnectedRun}`) || !line.includes('seq=2|')),
      'PS-A4 restarted the application or replayed the disconnected event.'
    );
    console.log('scenario PS-A4 passed');
  } finally {
    await subscriber.kill();
    await fault.close();
  }
}
