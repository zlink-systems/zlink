import assert from 'node:assert/strict';
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import { Limits, Scenario } from '../server/shared/constants.js';
export async function scenario(
  name: string,
  endpoint: string,
  drain: () => Promise<void>,
  stop: () => Promise<void>,
  cleanup: (release: () => Promise<void>) => Promise<void>
): Promise<void> {
  assert.equal(name, Scenario.connect);
  const connector = zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    waitTimeoutMs: Limits.messageMs,
    requestTimeoutMs: Limits.messageMs,
    reconnect: { enabled: false }
  });
  try {
    await connector.connect();
  } finally {
    await cleanup(() => connector.close());
  }
  await drain();
  await stop();
}
