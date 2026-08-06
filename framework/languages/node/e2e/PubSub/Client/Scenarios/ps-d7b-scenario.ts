// PS-D7B: Manual endpoint 변경은 automatic status를 바꾸지 않는다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import { getJson } from '../../../http-client';
import { PubSubNames } from '../../Shared/messages';
import { ensure } from '../Support/scenario-assert';
import { ServerProcessLauncher } from '../Support/server-process-launcher';
import { waitForEvent } from '../Support/subscriber-observation';
import { publishEvent } from './ps-a1-fanout-basic-delivery-scenario';

export async function runPsD7B(
  publisher: string,
  automaticSubscriber: string,
  manualSubscriber: string,
  processes: ServerProcessLauncher,
  publisherEndpoint: string
): Promise<void> {
  const manual = processes.startSubscriber(
    'sub-d7b-manual',
    manualSubscriber,
    'sub-d7b-manual.evidence.log',
    publisherEndpoint
  );
  try {
    await manual.waitReady();
    const before = randomUUID().replaceAll('-', '');
    await publishEvent(publisher, PubSubNames.mainTopic, before, 1, 'manual-before');
    await waitForEvent(automaticSubscriber, before, 1, 'manual-before', 10_000);
    const after = randomUUID().replaceAll('-', '');
    await publishEvent(publisher, PubSubNames.mainTopic, after, 2, 'manual-after');
    await waitForEvent(automaticSubscriber, after, 2, 'manual-after', 10_000);
    const evidence = await getJson<readonly string[]>(automaticSubscriber, '/evidence');
    ensure(evidence.some((line) => line.includes(`run=${before}`))
      && evidence.some((line) => line.includes(`run=${after}`)),
    'PS-D7B automatic subscriber did not receive both markers.');
    console.log('scenario PS-D7B passed');
  } finally {
    await manual.stop();
  }
}
