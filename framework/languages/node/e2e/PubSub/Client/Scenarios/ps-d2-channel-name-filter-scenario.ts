// PS-D2: 같은 ChannelName의 live fanout publisher만 선택한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { getJson } from '../../../http-client';
import { PubSubNames } from '../../Shared/messages';
import { ensure, eventually } from '../Support/scenario-assert';
import { waitForEvent, waitForNoEvent } from '../Support/subscriber-observation';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';

interface FanoutStatusWire {
  readonly isReady: boolean;
  readonly readyPublisherCount: number;
  readonly publishers: readonly {
    readonly nodeRid: string;
    readonly state: number;
  }[];
}

export async function runPsD2(
  eventsPublisher: string,
  auditPublisher: string | undefined,
  subscriber: string
): Promise<void> {
  ensure(auditPublisher !== undefined, 'PS-D2 requires a separate audit ChannelName publisher.');
  await eventually(
    async () => {
      const status = await getJson<FanoutStatusWire>(subscriber, '/status/fanout').catch(() => undefined);
      return status?.isReady === true
        && status.readyPublisherCount === 1
        && status.publishers.length === 1
        && status.publishers[0]?.nodeRid === 'pub-events'
        && status.publishers[0]?.state === 1;
    },
    'PS-D2 expected the events subscriber to select only the live events publisher.',
    20_000
  );

  const eventRunId = randomUUID().replaceAll('-', '');
  const auditRunId = randomUUID().replaceAll('-', '');
  await Promise.all([
    publishEvent(eventsPublisher, PubSubNames.mainTopic, eventRunId, 1, 'events-marker'),
    publishEvent(auditPublisher, PubSubNames.mainTopic, auditRunId, 1, 'audit-marker')
  ]);
  await waitForEvent(subscriber, eventRunId, 1, 'events-marker');
  await waitForNoEvent(subscriber, auditRunId, 1, 'audit-marker', 1_000);

  const status = await getJson<FanoutStatusWire>(subscriber, '/status/fanout');
  ensure(
    status.readyPublisherCount === 1
      && status.publishers.length === 1
      && status.publishers[0]?.nodeRid === 'pub-events',
    'PS-D2 expected the audit publisher and other topology descriptors to stay out of events status.'
  );
  console.log('scenario PS-D2 passed');
}
