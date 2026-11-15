'use strict';

const { createClient } = require('redis');

//  A job that provides Redis sets ZLINK_TEST_REDIS_URL. There an unreachable
//  Redis fails the test instead of skipping it; only a run without the variable
//  skips (#4542).
const providedRedisUrl = process.env.ZLINK_TEST_REDIS_URL;
const testRedisUrl = providedRedisUrl ?? 'redis://127.0.0.1:6379';

async function connectTestRedis(t) {
  const client = createClient({ url: testRedisUrl, socket: { reconnectStrategy: false } });
  client.on('error', () => {});
  try {
    await client.connect();
    await client.ping();
    return client;
  } catch (error) {
    if (client.isOpen) client.disconnect();
    if (providedRedisUrl !== undefined) {
      throw new Error(`ZLINK_TEST_REDIS_URL is set but Redis is unreachable at ${testRedisUrl}`, {
        cause: error
      });
    }
    t.skip(`Redis is unavailable at ${testRedisUrl}: ${error.message}`);
    return undefined;
  }
}

module.exports = { testRedisUrl, connectTestRedis };
