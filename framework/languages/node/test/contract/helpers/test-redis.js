'use strict';

const { createClient } = require('redis');

//  ZLINK_REDIS_TEST_ENDPOINT (host:port) is the one Redis a test run uses, in
//  every language. When it is set, an unreachable Redis fails the test; without
//  it a run tries 127.0.0.1:6379 and skips when nothing answers (#4542).
const providedEndpoint = process.env.ZLINK_REDIS_TEST_ENDPOINT;
const testRedisUrl = `redis://${providedEndpoint ?? '127.0.0.1:6379'}`;

async function connectTestRedis(t) {
  const client = createClient({ url: testRedisUrl, socket: { reconnectStrategy: false } });
  client.on('error', () => {});
  try {
    await client.connect();
    await client.ping();
    return client;
  } catch (error) {
    if (client.isOpen) client.disconnect();
    if (providedEndpoint !== undefined) {
      throw new Error(`ZLINK_REDIS_TEST_ENDPOINT is set but Redis is unreachable at ${testRedisUrl}`, {
        cause: error
      });
    }
    t.skip(`Redis is unavailable at ${testRedisUrl}: ${error.message}`);
    return undefined;
  }
}

module.exports = { testRedisUrl, connectTestRedis };
