const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');

test('node binding exposes the raw public API required by the framework runtime', () => {
  for (const name of [
    'createContext',
    'createDealerSocket',
    'createRouterSocket',
    'createPubSocket',
    'createSubSocket',
    'createStreamSocket'
  ]) {
    assert.equal(typeof zlink[name], 'function', `${name} must be public`);
  }
  for (const name of [
    'createMeshNode',
    'createDiscovery',
    'createRegistry',
    'createRegistryQueryClient'
  ]) {
    assert.equal(zlink[name], undefined, `${name} must not be public`);
  }
});

test('node binding public API covers raw socket monitor and stream wrappers', () => {
  const context = zlink.createContext();
  const closeables = [];

  try {
    const dealer = zlink.createDealerSocket(context);
    const router = zlink.createRouterSocket(context);
    const publisher = zlink.createPubSocket(context);
    const subscriber = zlink.createSubSocket(context);
    const stream = zlink.createStreamSocket(context);
    const monitor = dealer.monitorOpen();

    closeables.push(monitor, stream, subscriber, publisher, router, dealer);

    assert.equal(dealer.attachDiscovery, undefined);
    assert.equal(router.attachDiscovery, undefined);
    assert.equal(publisher.attachDiscovery, undefined);
    assert.equal(subscriber.attachDiscovery, undefined);
    assert.equal(typeof stream.setPacketHandler, 'function');
    assert.equal(typeof monitor.onEvent, 'function');
    assert.equal(typeof monitor.recv, 'function');
    assert.equal(typeof monitor.status, 'function');
  } finally {
    for (const closeable of closeables) {
      closeable.close();
    }
    context.close();
  }
});
