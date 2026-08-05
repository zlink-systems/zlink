// PS-E2A: Automatic subscriber의 Store 누락을 startup에서 거부한다 시나리오를 검증한다.
import { getStatus } from '../../../http-client';
import { eventually, ensure } from '../Support/scenario-assert';
import type { ServerProcessLauncher } from '../Support/server-process-launcher';

export async function runPsE2A(
  lateSubscriberUrl: string,
  processes: ServerProcessLauncher
): Promise<void> {
  const subscriber = processes.startSubscriber(
    'sub-e2a-no-store',
    lateSubscriberUrl,
    'sub-e2a-no-store.evidence.log'
  );
  try {
    await eventually(
      async () => subscriber.hasExited,
      'PS-E2A expected an automatic subscriber without a Store to terminate during startup.',
      10_000
    );
    ensure(
      subscriber.exitCode !== 0,
      'PS-E2A expected the invalid automatic subscriber process to terminate with an error.'
    );
    const healthStatus = await getStatus(`${lateSubscriberUrl}/health`).catch(() => 0);
    ensure(healthStatus !== 200, 'PS-E2A expected the invalid subscriber not to expose health.');
    console.log('scenario PS-E2A passed');
  } finally {
    if (!subscriber.hasExited) await subscriber.kill();
  }
}
