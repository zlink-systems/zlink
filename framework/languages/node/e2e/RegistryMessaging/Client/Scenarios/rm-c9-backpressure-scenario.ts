// RM-C9: Application HWM 도달 뒤 수신을 재개한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure, uniqueMarker } from '../Support/scenario-assert';

const slowSendCount = 8;

export async function runRmC9(backpressureConsumerUrl: string, providerAUrl: string): Promise<void> {
  await postJson(backpressureConsumerUrl, '/profile/backpressure/reset');
  const marker = uniqueMarker('rm-c9');
  const outcomes = await Promise.all(
    Array.from({ length: slowSendCount }, (_, index) =>
      postJson<string>(
        backpressureConsumerUrl,
        '/profile/backpressure/send',
        { commandId: `rm-c9-slow-${marker}-${index}` }
      ))
  );
  ensure(
    outcomes.every((outcome) => outcome === 'Submitted'),
    'RM-C9 expected all one-way sends to be submitted without a public bounded-failure oracle.'
  );

  const followUp = await waitForFollowUp(backpressureConsumerUrl);
  ensure(followUp.value === 'profile:rm-c9-after', 'RM-C9 follow-up request failed after backlog cleared.');

  const evidence = await postJson<string[]>(
    providerAUrl,
    '/evidence/wait',
    { contains: 'rm-c9-after', timeoutMilliseconds: 3000 }
  );
  ensure(
    evidence.some((line) => line.includes('rm-c9-after')),
    'RM-C9 recovery evidence missing.'
  );
  console.log('scenario RM-C9 passed');
}

async function waitForFollowUp(backpressureConsumerUrl: string): Promise<ProfileRes> {
  const deadline = performance.now() + 3000;
  let lastError: unknown;
  do {
    try {
      return await postJson<ProfileRes>(
        backpressureConsumerUrl,
        '/profile/request',
        { value: 'rm-c9-after' }
      );
    } catch (error) {
      lastError = error;
      await new Promise((resolve) => setTimeout(resolve, 25));
    }
  } while (performance.now() < deadline);
  throw new Error('RM-C9 follow-up request did not recover within 3000ms.', { cause: lastError });
}
