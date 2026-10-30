'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { randomUUID } = require('node:crypto');
const { Module, Injectable, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const n = require('../../packages/nestjs/dist');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const telemetry = require('./helpers/telemetry-log-capture');

class FirstRequest {}

test(
  'new ready host gets Existing Spot and sends its first remote request once',
  { timeout: 15000 },
  async () => {
    const store = new ZLinkInMemoryProviderLocationStore();
    const mesh = `first-${randomUUID()}`;
    const apps = [];
    let received = 0;
    class Room {
      configure() {}
      onCreate() {
        return { accepted: true };
      }
      onActorJoin() {
        return { accepted: false };
      }
      onJoinedActor() {}
      onLeaveActor() {}
    }
    Injectable({ scope: Scope.TRANSIENT })(Room);
    class FirstHandler {
      handle() {
        received++;
        return { received };
      }
    }
    n.zlinkSpotPacketHandler({ spot: () => Room, packetName: 'FirstRequest' })(FirstHandler);
    async function start(role) {
      const builder = n.zlinkFramework().addLocationStore(store);
      builder.configureDispatch().messageFlow('normal');
      builder
        .addRouteMesh(mesh)
        .listen('tcp://127.0.0.1:0')
        .routingId(`${role}-${mesh}`)
        .objects()
        .server()
        .addSpotFactory('Room', Room, (factory) => factory.disableRelocation());
      class AppModule {}
      Module({
        imports: [n.ZLinkModule.forRoot(builder.build())],
        providers: [Room, FirstHandler]
      })(AppModule);
      const app = await NestFactory.createApplicationContext(AppModule, {
        logger: false,
        abortOnError: false
      });
      apps.push(app);
      return app;
    }
    try {
      const owner = await start('owner');
      const created = await owner
        .get(n.ZLINK_SPOT_MANAGER)
        .getOrCreate('room', 'Room')
        .inMesh(mesh)
        .submit();
      assert.equal(created.state, framework.ZLinkSpotCreateState.Created);
      // Application-context startup completes the host READY barrier. No peer warm-up is used.
      const requester = await start('requester');
      const existing = await requester
        .get(n.ZLINK_SPOT_MANAGER)
        .getOrCreate('room', 'Room')
        .inMesh(mesh)
        .submit();
      assert.equal(existing.state, framework.ZLinkSpotCreateState.Existing);
      const reply = await requester
        .get(n.ZLINK_SPOT_OUTBOUND)
        .requestToSpot('room', new FirstRequest())
        .timeout(5000)
        .submit();
      assert.deepEqual(reply, { received: 1 });
      assert.equal(received, 1);
      await telemetry.observed();
      const terminals = telemetry.records.filter(
        (record) => record.packetName === 'FirstRequest' && record.phase === 'reply_received'
      );
      assert.equal(terminals.length, 1);
      assert.equal(terminals[0].outcome, 'succeeded');
      if (process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR !== undefined) {
        const fs = require('node:fs');
        const path = require('node:path');
        fs.writeFileSync(
          path.join(
            process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR,
            `${process.pid}.public-first.flow.jsonl`
          ),
          telemetry.records.map((record) => JSON.stringify(record)).join('\n') + '\n'
        );
      }
    } finally {
      for (const app of apps.reverse()) await app.close();
    }
  }
);
