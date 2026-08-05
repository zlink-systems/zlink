// PS-E2C: Automatic publisher identity 누락과 중복을 거부한다 시나리오를 검증한다.
import { getStatus } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';
import type { ServerProcessLauncher } from '../Support/server-process-launcher';

export async function runPsE2C(
  publisherUrl: string,
  secondPublisherUrl: string,
  missingEndpoint: string,
  bothEndpoint: string,
  redisEndpoint: string | undefined,
  redisKeyPrefix: string | undefined,
  processes: ServerProcessLauncher
): Promise<void> {
  ensure(redisEndpoint !== undefined && redisKeyPrefix !== undefined,
    'PS-E2C requires the dedicated Redis location store configuration.');
  ensure(missingEndpoint.length > 0 && bothEndpoint.length > 0,
    'PS-E2C requires two distinct publisher endpoints.');

  const missing = processes.startPublisherIdentityValidation(
    'pub-e2c-missing',
    publisherUrl,
    missingEndpoint,
    'missing',
    redisEndpoint,
    `${redisKeyPrefix}:missing`
  );
  const both = processes.startPublisherIdentityValidation(
    'pub-e2c-both',
    secondPublisherUrl,
    bothEndpoint,
    'both',
    redisEndpoint,
    `${redisKeyPrefix}:both`
  );

  try {
    await Promise.all([missing.waitForExit(10_000), both.waitForExit(10_000)]);
    ensure(missing.exitCode !== 0, 'PS-E2C expected the publisher without an identity mode to fail.');
    ensure(both.exitCode !== 0, 'PS-E2C expected the publisher with both identity modes to fail.');
    const [missingHealth, bothHealth] = await Promise.all([
      getStatus(`${publisherUrl}/health`).catch(() => 0),
      getStatus(`${secondPublisherUrl}/health`).catch(() => 0)
    ]);
    ensure(missingHealth !== 200, 'PS-E2C expected the missing-identity publisher not to expose health.');
    ensure(bothHealth !== 200, 'PS-E2C expected the duplicate-identity publisher not to expose health.');
    console.log('scenario PS-E2C passed');
  } finally {
    if (!missing.hasExited) await missing.kill();
    if (!both.hasExited) await both.kill();
  }
}
