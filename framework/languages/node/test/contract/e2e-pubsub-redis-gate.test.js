const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');
const pubSubRoot = path.join(nodeRoot, 'e2e/PubSub');

function read(relativePath) {
  return fs.readFileSync(path.join(pubSubRoot, relativePath), 'utf8');
}

test('PubSub keeps classic fanout descriptors independent from RouteMesh discovery', () => {
  const publisher = read('Server/Publisher/publisher-host-factory.ts');
  const subscriber = read('Server/Subscriber/subscriber-host-factory.ts');
  const runner = read('run_e2e.sh');
  const featureMap = read('feature-map.ko.md');

  for (const source of [publisher, subscriber]) {
    assert.doesNotMatch(source, /useInMemoryLocationStores/);
    assert.doesNotMatch(source, /addRouteMesh\(/);
    assert.doesNotMatch(source, /addClientServerChannel\(/);
    assert.match(source, /addFanoutChannel\(/);
  }
  assert.match(
    publisher,
    /if \(options\.redisEndpoint !== undefined && options\.redisKeyPrefix !== undefined\)[\s\S]*addLocationStore\(createRedisLocationStore\(/
  );
  assert.match(publisher, /options\.publisherIdentityMode/);
  assert.match(
    subscriber,
    /if \(options\.redisEndpoint !== undefined && options\.redisKeyPrefix !== undefined\)[\s\S]*addLocationStore\(createRedisLocationStore\(/
  );
  assert.match(subscriber, /\.enableSubscriber\(options\.publisherEndpoint\)/);

  assert.match(runner, /SCENARIO\^\^.*PS-D1[\s\S]*start_redis_container/);
  assert.match(runner, /SCENARIO\^\^.*PS-E2C[\s\S]*start_redis_container/);
  assert.match(runner, /start_configured_server pub-a[\s\S]*--publisher-endpoint "\$PUB_ENDPOINT"/);
  assert.match(featureMap, /classic fanout/i);
});
