'use strict';

const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const test = require('node:test');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const nest = require('../../packages/nestjs/dist');
const { fixedRidHostClock, finishFixedRidHostStartup } = require('../helpers/fixed-rid-host-clock');

test(
  'local and remote Spot sends and requests deliver metadata to the owner',
  { timeout: 15000 },
  async (t) => {
    const clock = fixedRidHostClock(t);
    const store = new ZLinkInMemoryProviderLocationStore();
    const mesh = `metadata-${randomUUID()}`;
    const apps = [];

    class Room {
      onCreate() {
        return { accepted: true };
      }
    }
    Injectable({ scope: Scope.TRANSIENT })(Room);

    class MetadataPacket {
      constructor(kind) {
        this.kind = kind;
      }
    }

    let resolveSend;
    class MetadataHandler {
      handle(_spot, message, context) {
        const value = context.metadata.find('trace-id') ?? null;
        if (message.kind === 'send') resolveSend(value);
        return { value };
      }
    }
    nest.zlinkSpotPacketHandler({ spot: () => Room, packetName: 'MetadataPacket' })(
      MetadataHandler
    );

    async function start(role) {
      const builder = nest.zlinkFramework().addLocationStore(store);
      builder.configureLocations().ownerLeaseFencingMarginMs(1);
      const objects = builder
        .addRouteMesh(mesh)
        .listen('tcp://127.0.0.1:0')
        .routingId(`${role}-${mesh}`)
        .objects();
      if (role === 'owner') {
        objects.server().addSpotFactory('Room', Room, (factory) => factory.disableRelocation());
      } else {
        objects.client();
      }
      class AppModule {}
      Module({
        imports: [nest.ZLinkModule.forRoot(builder.build())],
        providers: role === 'owner' ? [Room, MetadataHandler] : []
      })(AppModule);
      const app = await NestFactory.createApplicationContext(AppModule, {
        logger: false,
        abortOnError: false
      });
      apps.push(app);
      await finishFixedRidHostStartup(
        app.get(nest.ZLINK_FRAMEWORK_RUNTIME),
        clock,
        framework.ZLinkFrameworkRuntimeState.Preparing
      );
      return app;
    }

    try {
      const owner = await start('owner');
      const created = await owner
        .get(nest.ZLINK_SPOT_MANAGER)
        .getOrCreate('room', 'Room')
        .inMesh(mesh)
        .submit();
      assert.equal(created.state, framework.ZLinkSpotCreateState.Created);
      const peer = await start('peer');
      const routeMesh = peer.get(nest.ZLINK_ROUTE_MESH_RUNTIME);
      let ownerReady = false;
      for await (const observed of routeMesh.observe(mesh, 64, AbortSignal.timeout(10000))) {
        if (
          observed.status.peers.some(
            (peer) =>
              String(peer.nodeRid) === `owner-${mesh}` &&
              peer.state === framework.ZLinkPeerState.Ready
          )
        ) {
          ownerReady = true;
          break;
        }
      }
      assert.equal(ownerReady, true);
      for (const [scope, app] of [
        ['remote', peer],
        ['local', owner]
      ]) {
        const outbound = app.get(nest.ZLINK_SPOT_OUTBOUND);
        const delivered = new Promise((resolve) => {
          resolveSend = resolve;
        });
        await outbound
          .sendToSpot('room', new MetadataPacket('send'))
          .metadata('trace-id', scope)
          .submit();
        const reply = await outbound
          .requestToSpot('room', new MetadataPacket('request'))
          .metadata('trace-id', scope)
          .submit();
        assert.deepEqual([await delivered, reply.value], [scope, scope], scope);
      }
    } finally {
      for (const app of apps.reverse()) await app.close();
    }
  }
);
