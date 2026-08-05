// PS-E2B: Automatic과 manual mode를 한 registration에 섞으면 거부한다 시나리오를 검증한다.
import { getStatus } from '../../../http-client';
import { eventually, ensure } from '../Support/scenario-assert';
import type { ServerProcessLauncher } from '../Support/server-process-launcher';

export async function runPsE2B(
  lateSubscriberUrl: string,
  publisherEndpoint: string,
  processes: ServerProcessLauncher
): Promise<void> {
  const subscriber = processes.startSubscriber(
    'sub-e2b-mixed-mode',
    lateSubscriberUrl,
    'sub-e2b-mixed-mode.evidence.log',
    publisherEndpoint,
    'mixed'
  );
  try {
    await eventually(
      async () => subscriber.hasExited,
      'PS-E2B expected mixed automatic and manual subscriber sources to terminate during startup.',
      10_000
    );
    ensure(
      subscriber.exitCode !== 0,
      'PS-E2B expected the mixed subscriber process to terminate with a configuration error.'
    );
    const healthStatus = await getStatus(`${lateSubscriberUrl}/health`).catch(() => 0);
    ensure(healthStatus !== 200, 'PS-E2B expected the mixed subscriber not to expose health.');
    console.log('scenario PS-E2B passed');
  } finally {
    if (!subscriber.hasExited) await subscriber.kill();
  }
}
