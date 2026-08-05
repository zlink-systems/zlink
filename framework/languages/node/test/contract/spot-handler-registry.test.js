const assert = require('node:assert/strict');
const test = require('node:test');

const { DefaultZLinkSpotHandlerRegistry } = require('../../packages/framework/dist/runtime/spots');

test('repeated Spot subscription registration keeps one handler entry', () => {
  class PublishHandler {}
  const registry = new DefaultZLinkSpotHandlerRegistry();

  registry.addSubscribe(PublishHandler, 'mesh', 'topic');
  registry.addSubscribe(PublishHandler, 'mesh', 'topic');

  assert.deepEqual(registry.snapshot(), [
    { kind: 'subscribe', handlerType: PublishHandler, channelName: 'mesh', topic: 'topic' }
  ]);
});

test('different Spot subscription registrations remain independent', () => {
  class PublishHandler {}
  const registry = new DefaultZLinkSpotHandlerRegistry();

  registry.addSubscribe(PublishHandler, 'mesh', 'topic-a');
  registry.addSubscribe(PublishHandler, 'mesh', 'topic-b');
  registry.addSubscribe(PublishHandler, 'other-mesh', 'topic-a');

  assert.equal(registry.snapshot().length, 3);
});
