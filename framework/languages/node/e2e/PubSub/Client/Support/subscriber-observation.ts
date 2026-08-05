import { getJson, postJsonWithin } from '../../../http-client';
import { PubSubNames } from '../../Shared/messages';
import { delay } from './scenario-assert';

export async function waitForEvent(
  subscriberUrl: string,
  runId: string,
  sequence: number,
  value: string,
  timeoutMilliseconds = 10_000
): Promise<readonly string[]> {
  return await postJsonWithin<readonly string[]>(subscriberUrl, '/evidence/wait', {
    containsAllLineGroups: [[
      'event|',
      `run=${runId}`,
      `topic=${PubSubNames.mainTopic}`,
      `seq=${sequence}|`,
      `value=${value}`
    ]],
    timeoutMilliseconds
  }, timeoutMilliseconds + 1_000);
}

export async function waitForNoEvent(
  subscriberUrl: string,
  runId: string,
  sequence: number,
  value: string,
  timeoutMilliseconds = 1_000
): Promise<void> {
  const deadline = Date.now() + timeoutMilliseconds;
  while (Date.now() < deadline) {
    const evidence = await getJson<readonly string[]>(subscriberUrl, '/evidence');
    if (evidence.some((line) => line.includes('event|')
      && line.includes(`run=${runId}`)
      && line.includes(`seq=${sequence}|`)
      && line.includes(`value=${value}`))) {
      throw new Error(`Subscriber received event while it was expected to be disconnected: run=${runId}.`);
    }
    await delay(100);
  }
}
