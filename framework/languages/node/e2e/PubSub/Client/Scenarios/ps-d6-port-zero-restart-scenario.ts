// PS-D6: Port 0 재시작으로 endpoint가 바뀌어도 다시 연결한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { getJson, getStatus, postStatus } from '../../../http-client';
import { PubSubNames } from '../../Shared/messages';
import { ensure, eventually, isConnectionFailure } from '../Support/scenario-assert';
import { ServerProcessLauncher, type DynamicProcess } from '../Support/server-process-launcher';
import { waitForEvent } from '../Support/subscriber-observation';
import { hasReadyPublishers } from './ps-d3-publisher-set-convergence-scenario';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';

interface ListenerStatusWire {
  readonly channelName: string;
  readonly endpoint: string;
}

export async function runPsD6(
  publisherUrl: string,
  subscriberUrl: string,
  processes: ServerProcessLauncher
): Promise<void> {
  const publisher = processes.startPublisherNamed(
    'pub-d6',
    publisherUrl,
    'tcp://127.0.0.1:0',
    'pub-d6'
  );
  const subscriber = processes.startSubscriber(
    'sub-d6',
    subscriberUrl,
    'sub-d6.evidence.log'
  );
  let replacement: DynamicProcess | undefined;
  try {
    await Promise.all([publisher.waitReady(), subscriber.waitReady()]);
    await eventually(
      async () => hasReadyPublishers(subscriberUrl, ['pub-d6']),
      'PS-D6 expected the endpoint-less subscriber to discover the initial publisher.',
      20_000
    );

    const firstStatus = await getJson<ListenerStatusWire>(publisherUrl, '/status/listener');
    const firstPort = endpointPort(firstStatus.endpoint);
    ensure(firstStatus.channelName === PubSubNames.channel, 'PS-D6 returned an unexpected channel name.');
    ensure(firstPort > 0, `PS-D6 expected the initial actual port to be nonzero: ${firstStatus.endpoint}`);

    const beforeRestart = randomUUID().replaceAll('-', '');
    await publishEvent(publisherUrl, PubSubNames.mainTopic, beforeRestart, 1, 'before-port-zero-restart');
    await waitForEvent(subscriberUrl, beforeRestart, 1, 'before-port-zero-restart', 10_000);

    await postStatus(`${publisherUrl}/shutdown`);
    await eventually(async () => {
      try {
        return (await getStatus(`${publisherUrl}/health`)) !== 200;
      } catch (error) {
        return isConnectionFailure(error);
      }
    }, 'PS-D6 expected the initial publisher process to stop.');
    await publisher.waitForExit();

    replacement = processes.startPublisherNamed(
      'pub-d6-replacement',
      publisherUrl,
      'tcp://127.0.0.1:0',
      'pub-d6'
    );
    await replacement.waitReady();
    const secondStatus = await getJson<ListenerStatusWire>(publisherUrl, '/status/listener');
    const secondPort = endpointPort(secondStatus.endpoint);
    ensure(secondPort > 0, `PS-D6 expected the replacement actual port to be nonzero: ${secondStatus.endpoint}`);
    ensure(firstPort !== secondPort,
      `PS-D6 expected port 0 to allocate a new port, got ${firstPort} twice.`);

    await eventually(
      async () => hasReadyPublishers(subscriberUrl, ['pub-d6']),
      'PS-D6 expected the automatic subscriber to follow the replacement descriptor.',
      20_000
    );
    const afterRestart = randomUUID().replaceAll('-', '');
    await publishEvent(publisherUrl, PubSubNames.mainTopic, afterRestart, 2, 'after-port-zero-restart');
    await waitForEvent(subscriberUrl, afterRestart, 2, 'after-port-zero-restart', 10_000);
    console.log(`PS-D6 listener first=${firstStatus.endpoint} second=${secondStatus.endpoint}`);
    console.log('scenario PS-D6 passed');
  } finally {
    if (replacement !== undefined) await replacement.stop();
    if (!publisher.hasExited) await publisher.stop();
    await subscriber.stop();
  }
}

function endpointPort(endpoint: string): number {
  const parsed = new URL(endpoint);
  const port = Number(parsed.port);
  return Number.isInteger(port) ? port : 0;
}
