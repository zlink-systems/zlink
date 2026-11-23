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
  replace: () => Promise<void>
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
    console.log('▶ Connect to the Session node');
    await connector.connect();
  } finally {
    await connector.close();
  }
  console.log('▶ Drain the Session node');
  await drain();
  console.log('▶ Replace the Session node when it is safe to shut down');
  await replace();
}
