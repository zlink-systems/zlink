'use strict';

const assert = require('node:assert/strict');
const backend = require('../../../packages/framework/dist/runtime/backend');
const framework = require('../../../packages/framework/dist/internal');
const {
  ZLinkSubmitStatus
} = require('../../../packages/framework/dist/runtime/messaging/submission-result');

async function waitFor(condition, label) {
  const deadline = Date.now() + 2000;
  while (Date.now() < deadline) {
    if (condition()) return;
    await new Promise((resolve) => setImmediate(resolve));
  }
  throw new Error(`${label} timed out`);
}

async function main() {
  const backendFactory = new backend.ZLinkNodeBackendAdapterFactory();
  const context = backendFactory.createChannelAdapter().createContext();
  const meshName = `framework-publish-commit-${process.pid}`;
  const node = backendFactory.createMeshAdapter().createMeshNode(context, {
    meshName,
    routingId: meshName
  });
  node.setBind(`inproc://${meshName}`);
  node.addChannelName('events');
  node.start();
  const subscriber = node.entrySpot();
  subscriber.setSubscription('events', 'pre-start');
  subscriber.setSubscription('events', 'post-start');
  const publisher = node.createPublisher();
  const runtime = new framework.ZLinkSpotNodeRuntimeManager({
    registration: framework.createFrameworkRegistration({}),
    backendAdapterFactory: {},
    context: {}
  });
  runtime.publishers.set('play', publisher);

  try {
    const afterStart = new AbortController();
    const committed = runtime.publish(
      'play',
      'events',
      'post-start',
      'ProfileChanged',
      { sequence: 2 },
      afterStart.signal
    );
    await waitFor(
      () => node.status().pendingApplicationMessages > 0n,
      'Core Logical Multicast admission'
    );
    afterStart.abort(new Error('abort after Core start'));
    const result = await committed;
    assert.equal(result.status, ZLinkSubmitStatus.Submitted);
  } finally {
    subscriber.close();
    publisher.close();
    node.shutdown(1000);
    node.close();
    await context.dispose();
  }
}

main().then(
  () => process.stdout.write('logical-multicast-commit-ok\n'),
  (error) => {
    process.stderr.write(`${error?.stack ?? error}\n`);
    process.exitCode = 1;
  }
);
