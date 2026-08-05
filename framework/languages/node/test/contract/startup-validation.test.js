const assert = require('node:assert/strict');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');

const framework = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');

test('Application HWM rejects an unbounded Application listener', () => {
  assert.throws(
    () => framework.createFrameworkRegistration({
      inboundDispatch: { applicationHwmBytes: 1024n },
      channels: {
        api: {
          server: {
            bind: 'tcp://127.0.0.1:0',
            maxMessageSize: 0
          },
          requestHandlers: [{
            packetName: 'Ping',
            handler: { handle() { return { value: 'pong' }; } }
          }]
        }
      }
    }),
    /maxMessageSize must be bounded when Application HWM is enabled/
  );
});

test('Node registration rejects subscriber capability without matching handlers', () => {
  assert.throws(
    () => framework.createFrameworkRegistration(framework.createFrameworkOptions((builder) => {
      builder.addFanoutChannel('events').enableSubscriber('tcp://127.0.0.1:1');
    })),
    /subscriber must register at least one publish handler/
  );
});

test('Node registration rejects mixed automatic and manual fanout subscriber sources', () => {
  for (const automaticFirst of [true, false]) {
    assert.throws(
      () => framework.createFrameworkOptions((builder) => {
        const fanout = builder.addFanoutChannel(`events-${automaticFirst}`);
        if (automaticFirst) {
          fanout.enableSubscriber();
          fanout.enableSubscriber('tcp://127.0.0.1:7001');
        } else {
          fanout.enableSubscriber('tcp://127.0.0.1:7001');
          fanout.enableSubscriber();
        }
      }),
      /cannot combine automatic and manual subscriber sources/
    );
  }
});

test('Node registration requires an explicit identity mode for a Store-backed fanout publisher', () => {
  assert.throws(
    () => framework.createFrameworkRegistration(framework.createFrameworkOptions((builder) => {
      builder.addLocationStore(new framework.ZLinkInMemoryProviderLocationStore());
      builder.addFanoutChannel('events').enablePublisher();
    })),
    /publisher must select a fixed routing id or an automatic routing id prefix/
  );

  assert.doesNotThrow(() => framework.createFrameworkRegistration(framework.createFrameworkOptions((builder) => {
    builder.addLocationStore(new framework.ZLinkInMemoryProviderLocationStore());
    builder.addFanoutChannel('events')
      .enablePublisher()
      .setRoutingIdPrefix('events');
  })));

  assert.doesNotThrow(() => framework.createFrameworkRegistration(framework.createFrameworkOptions((builder) => {
    builder.addLocationStore(new framework.ZLinkInMemoryProviderLocationStore());
    builder.addFanoutChannel('events')
      .enablePublisher()
      .routingId('events-publisher');
  })));
});

test('Node module registration rejects mixed automatic and manual fanout subscriber sources', () => {
  assert.throws(
    () => nestjs.zlinkFramework()
      .addFanoutChannel('events')
        .enableSubscriber()
        .enableSubscriber('tcp://127.0.0.1:7001'),
    /cannot combine automatic and manual subscriber sources/
  );
});

test('Node module registration rejects subscriber capability without matching handlers', () => {
  assert.throws(
    () => nestjs.ZLinkModule.forRoot(nestjs.zlinkFramework()
      .addFanoutChannel('events')
        .enableSubscriber('tcp://127.0.0.1:1')
      .build()),
    /subscriber must register at least one publish handler/
  );
});

test('Node registration assigns an omitted RouteMesh listener to process-default port zero', () => {
  const registration = framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) => {
      builder.addRouteMesh('automatic');
    })
  );
  assert.equal(registration.spotNodes.get('automatic').router.bind, 'tcp://127.0.0.1:0');
  assert.equal(registration.spotNodes.get('automatic').router.port, 0);
});

test('RouteMesh listener uses separate bind and advertised hosts with Core-resolved port zero', () => {
  const options = framework.createFrameworkOptions((builder) => {
    builder.addRouteMesh('game')
      .setBindHost('0.0.0.0')
      .setAdvertiseHost('game.internal')
      .listen();
  });
  const registration = framework.createFrameworkRegistration(options);
  const router = registration.spotNodes.get('game').router;

  assert.equal(router.bind, 'tcp://0.0.0.0:0');
  assert.equal(router.bindHost, '0.0.0.0');
  assert.equal(router.advertiseHost, 'game.internal');
  assert.equal(router.port, 0);
});

test('Node listener rejects wildcard bind without a connectable advertise host', () => {
  assert.throws(
    () => framework.createFrameworkRegistration(framework.createFrameworkOptions((builder) => {
      builder.addRouteMesh('game')
        .setBindHost('0.0.0.0')
        .listen();
    })),
    /must define an advertise host when its bind host is a wildcard address/
  );

  assert.throws(
    () => framework.createFrameworkRegistration(framework.createFrameworkOptions((builder) => {
      builder.addRouteMesh('game')
        .listen('tcp://[::]:0');
    })),
    /must define an advertise host when its bind host is a wildcard address/
  );

  assert.throws(
    () => framework.createFrameworkRegistration(framework.createFrameworkOptions((builder) => {
      builder.addRouteMesh('game')
        .setBindHost('127.0.0.1')
        .setAdvertiseHost('0.0.0.0')
        .listen();
    })),
    /advertise host must identify a connectable host/
  );
});

test('RouteMesh Server membership does not require a local handler', () => {
  const registration = framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) => {
      builder.addRouteMesh('orders')
        .listen('tcp://127.0.0.1:0')
        .channel('orders')
        .server()
        .setWeight(0);
    })
  );

  assert.equal(registration.spotNodes.get('orders').meshChannels.orders.server, true);
  assert.equal(registration.spotNodes.get('orders').meshChannels.orders.weight, 0);
  assert.deepEqual(registration.spotNodes.get('orders').meshChannels.orders.requestHandlers, undefined);
  assert.deepEqual(registration.spotNodes.get('orders').meshChannels.orders.sendHandlers, undefined);
});

test('Location owner lease timing follows the TTL fencing relationship', () => {
  assert.throws(
    () => framework.createFrameworkRegistration({
      locations: {
        useInMemoryStores: true,
        options: {
          ownerLeaseRenewIntervalMs: 10_000,
          ownerLeaseTtlMs: 15_000,
          ownerLeaseFencingMarginMs: 5_000,
          ownerLeaseRenewTimeoutMs: 3_000
        }
      }
    }),
    /ownerLeaseRenewIntervalMs.*must be less than ownerLeaseTtlMs/
  );

  const registration = framework.createFrameworkRegistration({
    locations: {
      useInMemoryStores: true,
      options: {
        ownerLeaseRenewIntervalMs: 5_000,
        ownerLeaseTtlMs: 15_000,
        ownerLeaseFencingMarginMs: 5_000,
        ownerLeaseRenewTimeoutMs: 3_000
      }
    }
  });
  assert.equal(registration.locations.options.ownerLeaseRenewIntervalMs, 5_000);
});

test('RouteMesh channel registration rejects duplicate or mixed roles', () => {
  for (const build of [
    () => framework.createFrameworkOptions((builder) => {
      const channel = builder.addRouteMesh('orders').channel('orders');
      channel.client();
      channel.server();
    }),
    () => framework.createFrameworkOptions((builder) => {
      const channel = builder.addRouteMesh('orders').channel('orders');
      channel.server();
      channel.server();
    }),
    () => {
      const channel = nestjs.zlinkFramework().addRouteMesh('orders').channel('orders');
      channel.client();
      channel.server();
    }
  ]) {
    assert.throws(
      build,
      /RouteMesh channel must register exactly one role/
    );
  }

  assert.throws(
    () => framework.createFrameworkRegistration({
      spotNodes: {
        orders: {
          router: { bind: 'tcp://127.0.0.1:0' },
          meshChannels: { orders: { client: true, server: true } }
        }
      }
    }),
    /must register exactly one role/
  );
});

test('ClientServer listener applies the same wildcard advertise-host validation', () => {
  class NoticeHandler {}

  assert.throws(
    () => framework.createFrameworkRegistration(framework.createFrameworkOptions((builder) => {
      builder.addClientServerChannel('orders')
        .server()
        .setBindHost('0.0.0.0')
        .listen(9401)
        .addSendHandler(NoticeHandler);
    })),
    /must define an advertise host when its bind host is a wildcard address/
  );

  const registration = framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) => {
      builder.addClientServerChannel('orders')
        .server()
        .setBindHost('127.0.0.1')
        .listen(9401)
        .addSendHandler(NoticeHandler);
    })
  );
  assert.equal(registration.channels.get('orders').server.bind, 'tcp://127.0.0.1:9401');
});

test('process network defaults apply to every listener and listener overrides remain local', () => {
  class NoticeHandler {}
  class Session {}
  const registration = framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) => {
      const network = builder.configureNetwork();
      network.bindHost = '0.0.0.0';
      network.advertiseHost = 'node.internal';

      builder.addRouteMesh('game');
      builder.addRouteMesh('admin')
        .setBindHost('127.0.0.2')
        .setAdvertiseHost('admin.internal');
      builder.addClientServerChannel('orders')
        .server()
        .addSendHandler(NoticeHandler);
      builder.addFanoutChannel('events').enablePublisher();
      builder.addStreamNode('gateway').bind().registerSession(Session);
    })
  );

  assert.equal(registration.spotNodes.get('game').router.bind, 'tcp://0.0.0.0:0');
  assert.equal(registration.spotNodes.get('game').router.advertiseHost, 'node.internal');
  assert.equal(registration.spotNodes.get('admin').router.bind, 'tcp://127.0.0.2:0');
  assert.equal(registration.spotNodes.get('admin').router.advertiseHost, 'admin.internal');
  assert.equal(registration.channels.get('orders').server.bind, 'tcp://0.0.0.0:0');
  assert.equal(registration.channels.get('orders').server.advertiseHost, 'node.internal');
  assert.equal(registration.channels.get('events').publisher.bind, 'tcp://0.0.0.0:0');
  assert.equal(registration.channels.get('events').publisher.advertiseHost, 'node.internal');
  assert.equal(registration.streamNodes.get('gateway').bind, 'tcp://0.0.0.0:0');
  assert.equal(registration.streamNodes.get('gateway').advertiseHost, 'node.internal');
});

test('STREAM Actor dispatch is opt-in and requires global authority prerequisites', () => {
  class Session {}

  const streamOnly = framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) => {
      builder.addStreamNode('gateway').registerSession(Session);
    })
  );
  assert.equal(streamOnly.streamNodes.get('gateway').actorDispatchEnabled, undefined);

  assert.throws(
    () => framework.createFrameworkRegistration(framework.createFrameworkOptions((builder) => {
      builder.addRouteMesh('objects').objects().client();
      builder.addStreamNode('gateway').registerSession(Session).enableActorDispatch();
    })),
    /no Location Store is registered/
  );

  assert.throws(
    () => framework.createFrameworkRegistration(framework.createFrameworkOptions((builder) => {
      builder.addLocationStore(new framework.ZLinkInMemoryProviderLocationStore());
      builder.addStreamNode('gateway').registerSession(Session).enableActorDispatch();
    })),
    /at least one local Object Client or Server MeshNode/
  );

  const registration = framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) => {
      builder.addLocationStore(new framework.ZLinkInMemoryProviderLocationStore());
      builder.addRouteMesh('players').objects().client();
      builder.addRouteMesh('parties').objects().client();
      builder.addStreamNode('gateway').registerSession(Session).enableActorDispatch();
    })
  );
  assert.equal(registration.streamNodes.get('gateway').actorDispatchEnabled, true);
  assert.equal(registration.spotNodes.size, 2);
});

test('Object Client RouteMesh rejects application Node-direct handlers', () => {
  class NodeNotice {}
  assert.throws(
    () => framework.createFrameworkRegistration(
      framework.createFrameworkOptions((builder) => {
        const mesh = builder
          .addRouteMesh('client-only')
          .listen('tcp://127.0.0.1:0');
        mesh.objects().client();
        mesh.addRouteSendHandler(NodeNotice);
      })
    ),
    /Object Client RouteMesh 'client-only' cannot register Node-direct handlers/
  );
});

test('Node registration rejects invalid Spot timer options before startup', () => {
  class GameSpot {}
  class EntrySpot {}
  class TimerHandler {}
  const registrationWithTimer = (timerKind, timer) => ({
    spotNodes: {
      game: {
        router: { bind: 'tcp://127.0.0.1:0' },
        [timerKind]: [timer]
      }
    }
  });

  assert.throws(
    () => framework.createFrameworkRegistration(registrationWithTimer('spotTimerHandlers', {
      spotType: GameSpot,
      handlerType: TimerHandler,
      name: 'tick',
      periodMs: 0
    })),
    /period must be greater than zero/
  );
  assert.throws(
    () => framework.createFrameworkRegistration(registrationWithTimer('entrySpotTimerHandlers', {
      entrySpotType: EntrySpot,
      handlerType: TimerHandler,
      name: '',
      periodMs: 100
    })),
    /name must not be empty/
  );
  assert.throws(
    () => framework.createFrameworkRegistration(registrationWithTimer('spotTimerHandlers', {
      spotType: GameSpot,
      handlerType: TimerHandler,
      name: 'tick',
      periodMs: 100,
      options: { overrunPolicy: 'unsupported' }
    })),
    /overrun policy is not supported/
  );
  assert.throws(
    () => framework.createFrameworkRegistration(registrationWithTimer('spotTimerHandlers', {
      spotType: GameSpot,
      handlerType: TimerHandler,
      name: 'tick',
      periodMs: 100,
      options: {
        overrunPolicy: framework.ZLinkTimerOverrunPolicy.CatchUpBounded,
        maxCatchUpTicks: 0
      }
    })),
    /MaxCatchUpTicks must be greater than zero/
  );
});

test('Node one-way send timeout accepts only integer milliseconds in the public range', () => {
  const invalid = [0, -1, 1.5, Number.NaN, Number.POSITIVE_INFINITY, 2_147_483_648];
  for (const sendTimeoutMs of invalid) {
    assert.throws(
      () => framework.createFrameworkRegistration({
        channels: {
          api: {
            client: {
              manualConnections: ['tcp://127.0.0.1:7101'],
              sendTimeoutMs
            }
          }
        }
      }),
      /between 1 and 2147483647 milliseconds/
    );
    assert.throws(
      () => framework.createFrameworkRegistration({
        spotNodes: {
          play: {
            router: { bind: 'tcp://127.0.0.1:7102' },
            publisherConfig: { sendTimeoutMs }
          }
        }
      }),
      /between 1 and 2147483647 milliseconds/
    );
  }

  framework.createFrameworkRegistration({
    channels: {
      api: {
        client: {
          manualConnections: ['tcp://127.0.0.1:7101'],
          sendTimeoutMs: 2_147_483_647
        }
      }
    }
  });
});

test('Node live socket send timeout setter applies the same public range', () => {
  const socket = {
    peerWeight: 100,
    sendHighWaterMark: 1000,
    receiveHighWaterMark: 1000,
    sendTimeoutMs: 1000,
    maxMessageSize: 1024
  };
  const options = new framework.DefaultZLinkChannelRuntimeOptions(() => ({
    clientServerServerSocket() { return socket; },
    routeMeshSocket() { return socket; }
  }));
  const config = options.serverChannel('api');

  for (const sendTimeoutMs of [0, -1, 1.5, Number.NaN, Number.POSITIVE_INFINITY, 2_147_483_648]) {
    assert.throws(
      () => { config.sendTimeoutMs = sendTimeoutMs; },
      /between 1 and 2147483647 milliseconds/
    );
  }
  config.sendTimeoutMs = 2_147_483_647;
  assert.equal(socket.sendTimeoutMs, 2_147_483_647);
});

test('ClientServer role builders allow one Client and one Server for the same ChannelName', () => {
  class NoticeHandler {}
  class QueryHandler {}

  const options = framework.createFrameworkOptions((builder) => {
    builder.addClientServerChannel('orders')
      .client()
      .connect('tcp://127.0.0.1:9401');
    builder.addClientServerChannel('orders')
      .server()
      .setBindHost('0.0.0.0')
      .setAdvertiseHost('orders.internal')
      .listen(9401)
      .setWeight(75)
      .addSendHandler(NoticeHandler)
      .addRequestHandler(QueryHandler);
  });
  const registration = framework.createFrameworkRegistration(options);
  const channel = registration.channels.get('orders');

  assert.deepEqual(channel.client.manualConnections, ['tcp://127.0.0.1:9401']);
  assert.equal(channel.server.bind, 'tcp://0.0.0.0:9401');
  assert.equal(channel.server.advertiseHost, 'orders.internal');
  assert.equal(channel.server.weight, 75);
  assert.deepEqual(channel.sendHandlers.map((handler) => handler.handlerType), [NoticeHandler]);
  assert.deepEqual(channel.requestHandlers.map((handler) => handler.handlerType), [QueryHandler]);
  assert.deepEqual([...registration.channelClients], ['orders']);
});

test('ClientServer registration rejects duplicate topology names and repeated role selection', () => {
  assert.throws(() => framework.createFrameworkOptions((builder) => {
    builder.addClientServerChannel('orders');
    builder.addFanoutChannel('orders');
  }), /Duplicate channel 'orders'/);

  assert.throws(() => framework.createFrameworkOptions((builder) => {
    builder.addClientServerChannel('orders').client();
    builder.addClientServerChannel('orders').client();
  }), /Client role is already registered/);

  assert.throws(() => framework.createFrameworkOptions((builder) => {
    builder.addClientServerChannel('orders').server();
    builder.addClientServerChannel('orders').server();
  }), /Server role is already registered/);

  assert.throws(() => framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) => {
      builder.addClientServerChannel('orders').client().connect('tcp://127.0.0.1:9401');
      builder.addRouteMesh('mesh')
        .listen('tcp://127.0.0.1:0')
        .routingId('mesh-node')
        .channel('orders').server();
    })
  ), /registered on both RouteMesh and ClientServer physical paths/);
});

test('ClientServer registration allows repeated roles for different ChannelNames', () => {
  const registration = framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) => {
      builder.addClientServerChannel('orders').client().connect('tcp://127.0.0.1:9501');
      builder.addClientServerChannel('billing').client().connect('tcp://127.0.0.1:9502');
      builder.addClientServerChannel('shipping').server()
        .listen(9401)
        .addSendHandler(class ShippingHandler {});
      builder.addClientServerChannel('inventory').server()
        .listen(9402)
        .addSendHandler(class InventoryHandler {});
    })
  );

  assert.equal(registration.channels.get('orders').client !== undefined, true);
  assert.equal(registration.channels.get('billing').client !== undefined, true);
  assert.equal(registration.channels.get('shipping').server !== undefined, true);
  assert.equal(registration.channels.get('inventory').server !== undefined, true);
});

test('NestJS ClientServer builder preserves same-name Client and Server roles', () => {
  const builder = nestjs.zlinkFramework();
  builder.addClientServerChannel('orders').client().connect('tcp://127.0.0.1:9401');
  builder.addClientServerChannel('orders').server().listen(9401);
  const options = builder.build();

  assert.deepEqual(
    options.clientServerChannels.orders.client.manualConnections,
    ['tcp://127.0.0.1:9401']
  );
  assert.equal(options.clientServerChannels.orders.server.port, 9401);

  assert.throws(() => builder.addClientServerChannel('orders').client(), /Client role is already registered/);
  assert.throws(() => builder.addClientServerChannel('orders').server(), /Server role is already registered/);
});

async function assertNestStartupRejects(options, pattern) {
  class InvalidConfigurationModule {}
  Module({ imports: [nestjs.ZLinkModule.forRoot(options)] })(InvalidConfigurationModule);
  await assert.rejects(
    () => NestFactory.createApplicationContext(InvalidConfigurationModule, {
      logger: false,
      abortOnError: false
    }),
    pattern
  );
}
