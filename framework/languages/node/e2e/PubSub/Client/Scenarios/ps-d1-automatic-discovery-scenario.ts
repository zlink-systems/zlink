// PS-D1: Endpoint 없이 publisher를 발견한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { getJson } from '../../../http-client';
import { PubSubNames } from '../../Shared/messages';
import { eventually, ensure } from '../Support/scenario-assert';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';
import { waitForEvent } from '../Support/subscriber-observation';

interface FanoutStatusWire {
  readonly isReady: boolean;
  readonly readyPublisherCount: number;
  readonly publishers: readonly {
    readonly nodeRid: string;
    readonly state: number;
  }[];
}

export async function runPsD1(publisher: string, subscriber: string): Promise<void> {
  await eventually(
    async () => {
      const status = await getJson<FanoutStatusWire>(subscriber, '/status/fanout').catch(() => undefined);
      return status?.isReady === true
        && status.readyPublisherCount === 1
        && status.publishers.length === 1
        && status.publishers[0]?.nodeRid === 'pub-d1'
        && status.publishers[0]?.state === 1;
    },
    'PS-D1 expected an endpoint-less subscriber to discover the ready publisher from the Location Store.',
    20_000
  );

  const runId = randomUUID().replaceAll('-', '');
  await publishEvent(publisher, PubSubNames.mainTopic, runId, 1, 'automatic-discovery');
  await waitForEvent(subscriber, runId, 1, 'automatic-discovery', 10_000);
  ensure(
    (await getJson<FanoutStatusWire>(subscriber, '/status/fanout')).readyPublisherCount === 1,
    'PS-D1 expected the discovered publisher to remain ready after application delivery.'
  );
  console.log('scenario PS-D1 passed');
}
