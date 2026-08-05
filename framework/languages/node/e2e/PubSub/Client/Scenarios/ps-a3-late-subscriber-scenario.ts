// PS-A3: Late subscriber는 ready 이후 event부터 받는다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { PubSubNames } from '../../Shared/messages';
import { getJson } from '../../../http-client';
import { NetworkFaultProxy } from '../Support/network-fault-proxy';
import { ensure } from '../Support/scenario-assert';
import { ServerProcessLauncher } from '../Support/server-process-launcher';
import { publishEvent, publishUntilDelivered } from './ps-a1-fanout-basic-delivery-scenario';

export async function runPsA3(
  publisher: string,
  lateSubscriberUrl: string,
  processes: ServerProcessLauncher,
  publisherEndpoint: string
): Promise<void> {
  const beforeReadyRun = randomUUID().replaceAll('-', '');
  await publishEvent(publisher, PubSubNames.mainTopic, beforeReadyRun, 1, 'before-ready');

  const fault = await NetworkFaultProxy.start(publisherEndpoint, true);
  const subscriber = processes.startSubscriber(
    'sub-late', lateSubscriberUrl, 'sub-late.evidence.log', fault.endpoint
  );
  try {
    await subscriber.waitReady();
    fault.unblock();
    const afterReadyRun = await publishUntilDelivered(
      publisher, lateSubscriberUrl, PubSubNames.mainTopic, 2, 'after-ready'
    );
    const evidence = await getJson<readonly string[]>(lateSubscriberUrl, '/evidence');
    ensure(
      evidence.some((line) => line.includes(`run=${afterReadyRun}`))
        && evidence.every((line) => !line.includes(`run=${beforeReadyRun}`)),
      'PS-A3 late subscriber replayed before-ready or missed the first after-ready event.'
    );
    console.log('scenario PS-A3 passed');
  } finally {
    await subscriber.kill();
    await fault.close();
  }
}
