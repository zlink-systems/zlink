const assert = require('node:assert/strict');
const test = require('node:test');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');

function deferred() {
  let resolve;
  const promise = new Promise((complete) => {
    resolve = complete;
  });
  return { promise, resolve };
}

async function waitFor(condition) {
  for (let index = 0; index < 1000; index += 1) {
    if (condition()) return;
    await new Promise(setImmediate);
  }
  assert.fail('multicast delivery did not reach its expected boundary');
}

function pendingApplications(serial) {
  let count = 0;
  serial.visitPendingApplication(() => {
    count += 1;
  });
  return count;
}

for (const outcome of ['ready', 'rejected', 'initialize-error', 'commit-error']) {
  test(`Logical Multicast before Ready: ${outcome}`, { timeout: 15000 }, async () => {
    const entered = deferred();
    const release = deferred();
    const commitEntered = deferred();
    const commitRelease = deferred();
    const handled = [];
    const timerCalls = [];
    let spotId;
    class Changed {
      constructor(value) {
        this.value = value;
      }
    }
    class Subscription {
      handle(_spot, event) {
        handled.push(event);
      }
    }
    class Tick {
      handle() {
        timerCalls.push('tick');
      }
    }
    class Room {
      async configure() {
        this.context.handlers.addSubscribe(Subscription, 'events', 'changed');
        if (outcome === 'rejected') await this.context.addTimer('creation-tick', 1, Tick);
      }
      async onCreate() {
        spotId = this.context.spotId;
        if (outcome !== 'initialize-error') {
          entered.resolve();
          await release.promise;
        }
        return { accepted: outcome !== 'rejected' };
      }
      async onInitialize() {
        if (outcome === 'initialize-error') {
          entered.resolve();
          await release.promise;
          throw new Error('initialization failed');
        }
      }
    }
    Injectable({ scope: Scope.TRANSIENT })(Room);
    Injectable({ scope: Scope.TRANSIENT })(Subscription);
    Injectable({ scope: Scope.TRANSIENT })(Tick);
    const builder = nestjs.zlinkFramework().options({ locations: { useInMemoryStores: true } });
    const mesh = builder
      .addRouteMesh('pre-ready')
      .listen('tcp://127.0.0.1:0')
      .routingId('pre-ready-node');
    mesh.channel('events').server();
    mesh
      .objects()
      .server()
      .addSpotFactory('room', Room, (factory) => factory.disableRelocation());
    class AppModule {}
    Module({
      imports: [nestjs.ZLinkModule.forRoot(builder.build())],
      providers: [Room, Subscription, Tick]
    })(AppModule);
    const app = await NestFactory.createApplicationContext(AppModule, {
      logger: false,
      abortOnError: false
    });
    const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME);
    const spots = app.get(nestjs.ZLINK_SPOT_MANAGER);
    const publisher = app.get(nestjs.ZLINK_SPOT_PUBLISHER_CLIENT);
    const store = spots.options.coordinator.options.store;
    const commit = store.commit.bind(store);
    store.commit = async (...args) => {
      commitEntered.resolve();
      await commitRelease.promise;
      if (outcome === 'commit-error') throw new Error('Ready commit failed');
      return await commit(...args);
    };
    const received = [];
    const failures = [];
    const terminals = [];
    const closedParts = [];
    const dispatch = runtime.spotManager.dispatchMeshSpot.bind(runtime.spotManager);
    runtime.spotManager.dispatchMeshSpot = async (...args) => {
      received.push(args[2]);
      const counts = args[2].parts.map(() => 0);
      closedParts.push(counts);
      args[2].parts.forEach((part, index) => {
        const close = part.close.bind(part);
        part.close = () => {
          counts[index] += 1;
          close();
        };
      });
      try {
        return await dispatch(...args);
      } catch (error) {
        failures.push(error);
        throw error;
      } finally {
        terminals.push(args[2]);
      }
    };
    const creation = spots.create('room').inMesh('pre-ready').submit();
    const settled = creation.then(
      (value) => ({ value }),
      (error) => ({ error })
    );
    try {
      await Promise.race([
        entered.promise,
        settled.then((result) => {
          throw result.error ?? new Error('creation settled before the lifecycle hold');
        })
      ]);
      assert.equal(await spots.find(spotId), undefined);
      await publisher
        .publish('pre-ready', 'events', 'changed', new Changed('before-ready'))
        .submit();
      await waitFor(() => received.length === 1);
      await new Promise(setImmediate);
      assert.deepEqual(failures, [], 'matched pre-Ready target must retain its message');
      assert.equal(terminals.length, 0, 'delivery must remain owned until creation settles');
      assert.deepEqual(handled, []);
      const incarnation = runtime.spotManager.activations.activationForClose('pre-ready', spotId);
      if (outcome === 'rejected') {
        await waitFor(() => pendingApplications(incarnation.serial) === 2);
      } else assert.equal(pendingApplications(incarnation.serial), 1);
      assert.deepEqual(timerCalls, []);
      release.resolve();
      if (outcome === 'ready' || outcome === 'commit-error') {
        await commitEntered.promise;
        await new Promise(setImmediate);
        assert.equal(await spots.find(spotId), undefined);
        assert.equal(pendingApplications(incarnation.serial), 1);
        assert.equal(terminals.length, 0);
        assert.deepEqual(handled, []);
        commitRelease.resolve();
      }
      const result = await settled;
      if (outcome === 'initialize-error')
        assert.match(result.error?.message ?? '', /initialization failed/);
      else if (outcome === 'commit-error')
        assert.match(result.error?.message ?? '', /Store operation failed/);
      else assert.equal(result.error, undefined);
      if (outcome === 'ready' || outcome === 'rejected') {
        assert.equal(result.value.state, outcome === 'ready' ? 'created' : 'rejected');
      }
      await waitFor(() => terminals.length === 1);
      await new Promise(setImmediate);
      assert.deepEqual(failures, []);
      assert.deepEqual(handled, outcome === 'ready' ? [{ value: 'before-ready' }] : []);
      // The receive pump owns these messages and closes them at the real terminal.
      assert.ok(closedParts[0].length > 0);
      assert.ok(closedParts[0].every((count) => count === 1));
      await incarnation.serial.whenIdle();
      assert.equal(pendingApplications(incarnation.serial), 0);
      assert.deepEqual(timerCalls, []);
      if (outcome === 'ready') {
        assert.notEqual(await spots.find(spotId), undefined);
        await publisher
          .publish('pre-ready', 'events', 'changed', new Changed('after-ready'))
          .submit();
        await waitFor(() => handled.length === 2);
        assert.deepEqual(handled, [{ value: 'before-ready' }, { value: 'after-ready' }]);
      } else assert.equal(await spots.find(spotId), undefined);
    } finally {
      release.resolve();
      commitRelease.resolve();
      await settled;
      await app.close();
    }
  });
}
