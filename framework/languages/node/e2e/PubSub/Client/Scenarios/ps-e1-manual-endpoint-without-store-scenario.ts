// PS-E1: Store 없이 manual endpoint만 사용한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { PubSubNames } from '../../Shared/messages';
import { getJson } from '../../../http-client';
import { NetworkFaultProxy } from '../Support/network-fault-proxy';
import { ensure } from '../Support/scenario-assert';
import { ServerProcessLauncher } from '../Support/server-process-launcher';
import { publishEvent, publishUntilDelivered } from './ps-a1-fanout-basic-delivery-scenario';

export async function runPsE1(
  publisher: string,
  lateSubscriberUrl: string,
  processes: ServerProcessLauncher,
  publisherEndpoint: string
): Promise<void> {
  const beforeReadyRun = randomUUID().replaceAll('-', '');
  const fault = await NetworkFaultProxy.start(publisherEndpoint, true);
  const subscriber = processes.startSubscriber(
    'sub-e1-manual', lateSubscriberUrl, 'sub-e1-manual.evidence.log', fault.endpoint
  );
  try {
    await subscriber.waitReady();
    await publishEvent(publisher, PubSubNames.mainTopic, beforeReadyRun, 1, 'before-late');
    fault.unblock();
    const afterReadyRun = await publishUntilDelivered(
      publisher, lateSubscriberUrl, PubSubNames.mainTopic, 2, 'after-ready'
    );
    const evidence = await getJson<readonly string[]>(lateSubscriberUrl, '/evidence');
    ensure(
      evidence.some((line) => line.includes(`run=${afterReadyRun}`))
        && evidence.every((line) => !line.includes(`run=${beforeReadyRun}`)),
      'PS-E1 manual subscriber replayed before-late or missed after-ready event.'
    );
    console.log('scenario PS-E1 passed');
  } finally {
    await subscriber.kill();
    await fault.close();
  }
}
