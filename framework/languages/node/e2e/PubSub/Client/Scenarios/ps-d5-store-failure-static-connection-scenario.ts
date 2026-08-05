// PS-D5: Store 장애 중 기존 connection을 유지하고 복구한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { getJson } from '../../../http-client';
import { PubSubNames } from '../../Shared/messages';
import { ensure, eventually } from '../Support/scenario-assert';
import { NetworkFaultProxy } from '../Support/network-fault-proxy';
import { ServerProcessLauncher } from '../Support/server-process-launcher';
import { waitForEvent } from '../Support/subscriber-observation';
import { hasReadyPublishers } from './ps-d3-publisher-set-convergence-scenario';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';

interface LocationStatusWire {
  readonly storeHealthy: boolean;
  readonly ownerLeaseHealthy: boolean;
  readonly lastError?: string;
}

export async function runPsD5(
  publisherUrl: string,
  subscriberUrl: string,
  redisEndpoint: string | undefined,
  redisKeyPrefix: string | undefined,
  processes: ServerProcessLauncher
): Promise<void> {
  ensure(redisEndpoint !== undefined && redisKeyPrefix !== undefined,
    'PS-D5 requires a dedicated Redis location store.');
  const storeFault = await NetworkFaultProxy.start(redisEndpoint);
  const publisher = processes.startPublisherNamed(
    'pub-d5',
    publisherUrl,
    'tcp://127.0.0.1:0',
    'pub-d5'
  );
  const subscriber = processes.startSubscriberWithRedis(
    'sub-d5',
    subscriberUrl,
    'sub-d5.evidence.log',
    storeFault.redisEndpoint,
    redisKeyPrefix
  );
  try {
    await Promise.all([publisher.waitReady(), subscriber.waitReady()]);
    await eventually(
      async () => hasReadyPublishers(subscriberUrl, ['pub-d5']),
      'PS-D5 expected the publisher connection to become ready before Store failure.',
      20_000
    );
    const baselineLocationStatus = await getJson<LocationStatusWire>(subscriberUrl, '/location/status');
    ensure(
      baselineLocationStatus.storeHealthy,
      'PS-D5 expected the subscriber Location Store status to be healthy before fault injection.'
    );
    const baselineRun = randomUUID().replaceAll('-', '');
    await publishEvent(publisherUrl, PubSubNames.mainTopic, baselineRun, 1, 'before-store-failure');
    await waitForEvent(subscriberUrl, baselineRun, 1, 'before-store-failure', 10_000);

    storeFault.block();
    await eventually(
      async () => (await getJson<LocationStatusWire>(subscriberUrl, '/location/status').catch(() => undefined))
        ?.storeHealthy === false,
      'PS-D5 expected the subscriber Location Store status to become unhealthy while the Store proxy is blocked.',
      20_000
    );
    const degradedRun = randomUUID().replaceAll('-', '');
    await publishEvent(publisherUrl, PubSubNames.mainTopic, degradedRun, 2, 'during-store-failure');
    await waitForEvent(subscriberUrl, degradedRun, 2, 'during-store-failure', 10_000);
    ensure(
      await hasReadyPublishers(subscriberUrl, ['pub-d5']),
      'PS-D5 expected Store failure not to remove an already-ready publisher connection.'
    );

    storeFault.unblock();
    await eventually(
      async () => {
        const status = await getJson<LocationStatusWire>(subscriberUrl, '/location/status').catch(() => undefined);
        return status?.storeHealthy === true && status.lastError === undefined;
      },
      'PS-D5 expected the subscriber Location Store status to recover without replacing the connection.',
      20_000
    );
    ensure(
      await hasReadyPublishers(subscriberUrl, ['pub-d5']),
      'PS-D5 expected the recovered Store view to retain the same ready publisher connection.'
    );
    const recoveredRun = randomUUID().replaceAll('-', '');
    await publishEvent(publisherUrl, PubSubNames.mainTopic, recoveredRun, 3, 'after-store-recovery');
    await waitForEvent(subscriberUrl, recoveredRun, 3, 'after-store-recovery', 10_000);
    console.log('scenario PS-D5 passed');
  } finally {
    await subscriber.stop();
    await publisher.stop();
    await storeFault.close();
  }
}
