const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const backend = require('../../packages/framework/dist/runtime/backend');
const {
  wrapSocket
} = require('../../packages/framework/dist/runtime/backend/node/node-socket-backend-adapter');
const {
  ZLinkChannelSocketRegistry
} = require('../../packages/framework/dist/runtime/channels/channel-socket-registry');

test('Nest fanout timeout validates immediately and reaches Framework registration', () => {
  const nestjs = require('../../packages/nestjs/dist');
  const { createRegistrationOptions } = require('../../packages/nestjs/dist/registration-composer');
  for (const value of [0, -1, 1.5, NaN, Infinity, 2147483648]) {
    assert.throws(
      () => nestjs.zlinkFramework().addFanoutChannel('events').setSendTimeout(value),
      framework.ZLinkConfigurationException
    );
  }
  const builder = nestjs.zlinkFramework();
  builder.addFanoutChannel('first').setSendTimeout(75).enablePublisher(0);
  builder.addFanoutChannel('second').enablePublisher(0).setSendTimeout(625);
  const registration = framework.createFrameworkRegistration(
    createRegistrationOptions(builder.build())
  );
  assert.equal(registration.channels.get('first').sendTimeoutMs, 75);
  assert.equal(registration.channels.get('second').sendTimeoutMs, 625);
  const subscriber = nestjs.zlinkFramework();
  subscriber.addFanoutChannel('events').enableSubscriber().setSendTimeout(75);
  assert.throws(
    () => framework.createFrameworkRegistration(createRegistrationOptions(subscriber.build())),
    framework.ZLinkConfigurationException
  );
});

test('Nest default NoDrop setter enables NoDrop on its own channel', () => {
  const nestjs = require('../../packages/nestjs/dist');
  const builder = nestjs.zlinkFramework();
  builder.addFanoutChannel('default').enablePublisher(0);
  builder.addFanoutChannel('enabled').enablePublisher(0).setNoDrop();
  builder.addFanoutChannel('disabled').enablePublisher(0).setNoDrop(false);
  const options = builder.build().fanoutChannels;
  assert.equal(options.default.noDrop, undefined);
  assert.equal(options.enabled.noDrop, true);
  assert.equal(options.disabled.noDrop, false);
});

test('public fanout builders apply independent publisher settings without HWM writes', async () => {
  const registration = framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) => {
      builder.addFanoutChannel('default').enablePublisher(0);
      builder.addFanoutChannel('first').setSendTimeout(75).enablePublisher(0).setNoDrop();
      builder.addFanoutChannel('second').enablePublisher(0).setSendTimeout(625).setNoDrop(false);
    })
  );
  const publishers = [];
  const adapter = {
    createPublisherSocket() {
      const socket = {
        nativeInstance: {},
        lingerMs: 0,
        set sendHighWaterMark(value) {
          assert.fail(`Unexpected Framework HWM write: ${value}`);
        },
        setChannelName() {},
        bind() {},
        publish() {},
        async dispose() {}
      };
      publishers.push(socket);
      return socket;
    }
  };
  const registry = new ZLinkChannelSocketRegistry(registration, adapter, {});
  try {
    for (const [channel, timeout, noDrop] of [
      ['default', 1000, false],
      ['first', 75, true],
      ['second', 625, false]
    ]) {
      const socket = registry.publisher(channel);
      assert.equal(socket.sendTimeoutMs, timeout);
      assert.equal(socket.noDrop, noDrop);
      assert.equal(socket.lingerMs, 0);
    }
    assert.equal(publishers.length, 3);
  } finally {
    await registry.dispose();
  }
});

test('public fanout timeout setter rejects invalid values immediately', () => {
  for (const value of [0, -1, 1.5, NaN, Infinity, 2147483648]) {
    assert.throws(
      () =>
        framework.createFrameworkOptions((builder) =>
          builder.addFanoutChannel('events').setSendTimeout(value)
        ),
      framework.ZLinkConfigurationException
    );
  }
  assert.doesNotThrow(() =>
    framework.createFrameworkRegistration(
      framework.createFrameworkOptions((builder) =>
        builder.addFanoutChannel('events').setSendTimeout(2147483647).enablePublisher(0)
      )
    )
  );
});

test('publisher settings without the publisher role fail startup', () => {
  for (const configure of [
    (channel) => channel.setSendTimeout(75),
    (channel) => channel.setNoDrop(false)
  ]) {
    const options = framework.createFrameworkOptions((builder) =>
      configure(builder.addFanoutChannel('events'))
    );
    assert.throws(
      () => framework.createFrameworkRegistration(options),
      framework.ZLinkConfigurationException
    );
  }
});

test('close and dispose preserve the linger selected when each socket was created', async () => {
  for (const method of ['close', 'dispose']) {
    for (const linger of [0, 30000]) {
      let writes = 0;
      let selectedLinger = -1;
      let closed = false;
      const socket = wrapSocket({
        options: {
          get linger() {
            return selectedLinger;
          },
          set linger(value) {
            writes++;
            selectedLinger = value;
          }
        },
        close() {
          closed = true;
        }
      });
      assert.equal(selectedLinger, 0);
      assert.equal(writes, 1);
      selectedLinger = linger;
      await socket[method]();
      assert.equal(closed, true);
      assert.equal(selectedLinger, linger);
      assert.equal(writes, 1);
    }
  }
});

test('all native socket roles receive zero linger at creation', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const adapter = factory.createChannelAdapter();
  const context = adapter.createContext();
  const sockets = [];
  try {
    for (const create of [
      () => adapter.createRouterSocket(context),
      () => adapter.createDealerSocket(context),
      () => adapter.createSubscriberSocket(context),
      () => adapter.createPublisherSocket(context),
      () => factory.createStreamAdapter().createStreamSocket(context)
    ]) {
      const socket = create();
      sockets.push(socket);
      assert.equal(socket.nativeInstance.options.linger, 0);
    }
  } finally {
    for (const socket of sockets) await socket.dispose();
    await context.dispose();
  }
});

test('native fanout publisher receives the channel timeout, NoDrop and zero linger', async () => {
  const registration = framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) =>
      builder.addFanoutChannel('events').enablePublisher(0).setSendTimeout(75).setNoDrop()
    )
  );
  const adapter = new backend.ZLinkNodeBackendAdapterFactory().createChannelAdapter();
  const context = adapter.createContext();
  const registry = new ZLinkChannelSocketRegistry(registration, adapter, context);
  try {
    const socket = registry.publisher('events').nativeInstance;
    assert.equal(socket.options.sendTimeout, 75);
    assert.equal(socket.options.noDrop, true);
    assert.equal(socket.options.linger, 0);
  } finally {
    await registry.dispose();
    await context.dispose();
  }
});
