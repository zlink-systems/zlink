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
  control?: { drain: () => Promise<void>; stop: () => Promise<void> }
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
  } catch (error) {
    try {
      await connector.close();
    } catch (cleanup) {
      throw new AggregateError([error, cleanup], 'STREAM connection and cleanup failed');
    }
    throw error;
  }
  await connector.close();
  await control?.drain();
  await control?.stop();
}
