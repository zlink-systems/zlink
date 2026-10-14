const assert = require('node:assert/strict');
const test = require('node:test');
const { startServer } = require('./helpers/http-server-fixture');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const fixtures = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');
const telemetry = require('./helpers/telemetry-log-capture');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

function memoryBlobs() {
  const blobs = new Map();
  const live = (reference) => {
    const value = blobs.get(reference.value);
    if (value && value.expiresAt.getTime() <= Date.now()) blobs.delete(reference.value);
    return blobs.get(reference.value);
  };
  return {
    async put(reference, bytes, retentionMs) {
      const storeNow = new Date();
      const previous = live(reference);
      if (previous)
        return {
          kind:
            Buffer.compare(previous.bytes, Buffer.from(bytes)) === 0 ? 'alreadyStored' : 'conflict',
          storeNow,
          expiresAt: previous.expiresAt
        };
      const value = { bytes: Buffer.from(bytes), expiresAt: new Date(Date.now() + retentionMs) };
      blobs.set(reference.value, value);
      return { kind: 'stored', storeNow, expiresAt: value.expiresAt };
    },
    async read(reference) {
      const value = live(reference);
      return value
        ? { kind: 'found', ...value, storeNow: new Date() }
        : { kind: 'missing', storeNow: new Date() };
    },
    async renew(reference, retentionMs) {
      const value = live(reference);
      if (!value) return { kind: 'missing', storeNow: new Date() };
      value.expiresAt = new Date(Date.now() + retentionMs);
      return { kind: 'renewed', expiresAt: value.expiresAt, storeNow: new Date() };
    },
    async delete(reference) {
      blobs.delete(reference.value);
    }
  };
}

for (const callbackFailure of [false, true]) {
  test(`public Instance HTTP Yield shutdown and replacement host (callback failure=${callbackFailure})`, async () => {
    telemetry.reset();
    const entered = deferred();
    const release = deferred();
    const original = deferred();
    let ended = 0;
    let sourceContext;
    let uri;
    class ProbeRequest {
      constructor(value) {
        this.value = value;
      }
    }
    framework.ZLinkPacket('ShutdownProbe')(ProbeRequest);
    class Probe {
      async onClosing() {
        if (callbackFailure) throw new Error('application close failed');
      }
    }
    class Handler {
      async handle(spot, request) {
        if (request.value !== 'hold') return request.value;
        try {
          return await spot.context
            .runIoWorker(() => {
              const pending = fetch(uri).then((response) => response.text());
              pending.then(original.resolve);
              return pending;
            })
            .yield();
        } finally {
          ended += 1;
        }
      }
    }

    Injectable({ scope: Scope.TRANSIENT })(Probe);
    nestjs.zlinkSpotPacketHandler({ spot: () => Probe, packetName: 'ShutdownProbe' })(Handler);
    const server = await startServer(async (_request, response) => {
      entered.resolve();
      await release.promise;
      response.end('late');
    });
    uri = `${server.baseUrl}/generate`;
    const ownerLeaseRenewTimeoutMs = 500;
    const store = new fixtures.ZLinkInMemoryProviderLocationStore();
    const blobs = memoryBlobs();
    const apps = [];
    async function start(name, target) {
      const builder = nestjs.zlinkFramework();
      builder.addLocationStore(store).addRelocationStore(blobs);
      builder.configureDispatch().messageFlow('normal');
      builder
        .configureLocations()
        .pollingIntervalMs(20)
        .ownerLeaseRenewTimeoutMs(ownerLeaseRenewTimeoutMs);
      const mesh = builder
        .addRouteMesh('shutdown-http')
        .listen('tcp://127.0.0.1:0')
        .routingId(`${name}-${process.pid}-${callbackFailure}`);
      if (target)
        mesh
          .objects()
          .server()
          .addInstanceSpotFactory('probe', Probe, (factory) => factory.disableRelocation());
      else mesh.objects().client();
      class App {}
      Module({
        imports: [nestjs.ZLinkModule.forRoot(builder.build())],
        providers: target ? [Probe, Handler] : []
      })(App);
      const app = await NestFactory.createApplicationContext(App, {
        logger: false,
        abortOnError: false
      });
      apps.push(app);
      if (!target) sourceContext = { outbound: app.get(nestjs.ZLINK_SPOT_OUTBOUND) };
      return app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME);
    }
    try {
      const source = await start('source', false);
      const target = await start('target', true);
      async function ready(targetRuntime, name) {
        const end = Date.now() + 5000;
        while (
          !source.status.isReady ||
          !targetRuntime.status.isReady ||
          !targetRuntime.routeMeshRuntime
            .snapshot('shutdown-http')
            .peers.some((peer) => peer.state === framework.ZLinkPeerState.Ready) ||
          !source.routeMeshRuntime
            .snapshot('shutdown-http')
            .peers.some(
              (peer) =>
                peer.state === framework.ZLinkPeerState.Ready &&
                String(peer.nodeRid) === `${name}-${process.pid}-${callbackFailure}`
            )
        ) {
          if (Date.now() >= end) throw new Error('RouteMesh hosts never became ready');
          await new Promise((resolve) => setTimeout(resolve, 2));
        }
      }
      await ready(target, 'target');
      const id = `probe-${process.pid}-${callbackFailure}`;
      const request = sourceContext.outbound
        .requestToSpot(id, new ProbeRequest(callbackFailure ? 'echo' : 'hold'))
        .instanceSpot('probe')
        .inMesh('shutdown-http')
        .timeout(5000)
        .submit();
      void request.catch(() => undefined);
      if (callbackFailure) assert.equal(await request, 'echo');
      else
        await Promise.race([
          entered.promise,
          request.then(() => {
            throw new Error('request completed before HTTP arrival');
          })
        ]);
      const deadlineMs = callbackFailure ? 2000 : 250;
      const observationMs = deadlineMs + ownerLeaseRenewTimeoutMs + 1000;
      let observer;
      let result;
      try {
        result = await Promise.race([
          target.shutdown({ deadlineMs }),
          new Promise((_, reject) => {
            observer = setTimeout(
              () => reject(new Error('Shutdown exceeded its specified teardown end.')),
              observationMs
            );
          })
        ]);
      } finally {
        clearTimeout(observer);
      }
      assert.equal(result.outcome, framework.ZLinkFrameworkTerminationOutcome.ForceStopped);
      assert.equal(
        result.reason,
        callbackFailure
          ? framework.ZLinkFrameworkTerminationReason.TeardownFailed
          : framework.ZLinkFrameworkTerminationReason.DeadlineExceeded
      );
      if (!callbackFailure) {
        assert.equal(ended, 1, 'the owned wait ends before shutdown returns');
        release.resolve();
        assert.equal(await original.promise, 'late');
        assert.equal(ended, 1);
        assert.equal(
          telemetry.records.filter((record) => record.reason === 'handler_exception').length,
          0
        );
        const replacement = await start('replacement', true);
        await ready(replacement, 'replacement');
        assert.equal(
          await sourceContext.outbound
            .requestToSpot(id, new ProbeRequest('echo'))
            .instanceSpot('probe')
            .inMesh('shutdown-http')
            .timeout(5000)
            .submit(),
          'echo'
        );
      }
    } finally {
      release.resolve();
      const results = await Promise.allSettled(apps.reverse().map((app) => app.close()));
      server.server.closeAllConnections();
      await server.close();
      const failures = results.filter((result) => result.status === 'rejected');
      assert.equal(failures.length, 0, failures.map((result) => String(result.reason)).join('\n'));
    }
  });
}

test('Shutdown classification preserves Framework cancellation origin and application exceptions', () => {
  const {
    hasTeardownFailure
  } = require('../../packages/framework/dist/runtime/host/runtime-shutdown');
  const ended = fixtures.createInternalFrameworkException(
    fixtures.ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
    'Runtime-owned wait ended.'
  );
  assert.equal(hasTeardownFailure(ended), false);
  assert.equal(
    hasTeardownFailure(new framework.ZLinkFrameworkException(ended.kind, 'Application failure.')),
    true
  );
});
