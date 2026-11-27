require('./helpers/telemetry-log-capture');

const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const connector = require('../../packages/stream-connector/dist');
const framework = require('../../packages/framework/dist/internal');
const nest = require('../../packages/nestjs/dist');
const {
  AdmissionQueueSpot
} = require('../../packages/framework/dist/runtime/streams/admission-queue-spot');
const { MemoryRelocationStore } = require('./fixtures/instance-relocation-memory-store');
const deferred = require('./helpers/deferred.cjs');
const { fixedRidHostClock, finishFixedRidHostStartup } = require('../helpers/fixed-rid-host-clock');

test(
  'one Instance Spot permit limits concurrent authentication across two real nodes',
  {
    timeout: 25_000
  },
  async (t) => {
    const clock = fixedRidHostClock(t);
    const store = new framework.ZLinkInMemoryProviderLocationStore();
    const relocationStore = new MemoryRelocationStore();
    const mesh = `login-${randomUUID()}`;
    const key = Buffer.alloc(32, 17).toString('base64url');
    const apps = [];
    const clients = [];
    const firstEntered = deferred();
    const firstRelease = deferred();
    const prematurePermitRequest = deferred();
    const waitingReport = deferred();
    const paths = [];
    let current = 0;
    let maximum = 0;
    let firstNodeRid;
    const admit = AdmissionQueueSpot.prototype.admit;
    t.mock.method(AdmissionQueueSpot.prototype, 'admit', function (request) {
      firstNodeRid ??= request.nodeRid;
      if (request.nodeRid !== firstNodeRid && current !== 0) {
        prematurePermitRequest.resolve(request);
      }
      return admit.call(this, request);
    });
    const report = AdmissionQueueSpot.prototype.report;
    t.mock.method(
      AdmissionQueueSpot.prototype,
      'report',
      async function (request, reclaimEndedLeases) {
        const reply = await report.call(this, request, reclaimEndedLeases);
        if (firstNodeRid !== undefined && request.nodeRid !== firstNodeRid) {
          const waiters = this.waiters().filter(
            ({ connection }) => connection.nodeRid === request.nodeRid
          );
          if (waiters.length === 2) waitingReport.resolve(waiters.map(({ ticket }) => ticket));
        }
        return reply;
      }
    );
    const abort = new AbortController();

    async function start(node) {
      class SessionFactory {
        async create(context) {
          return {
            context,
            async onDispatch(dispatch, payload) {
              assert.equal(dispatch.packetName, 'Login');
              const { id } = payload.decode();
              paths.push({ id, node });
              maximum = Math.max(maximum, ++current);
              try {
                if (id === 'held') {
                  firstEntered.resolve();
                  await firstRelease.promise;
                }
                await context.client.reply({ id }).submit();
              } finally {
                current--;
              }
            }
          };
        }
      }
      const builder = nest
        .zlinkFramework()
        .addLocationStore(store)
        .addRelocationStore(relocationStore);
      builder.configureDispatch().messageFlow('normal');
      builder.configureLocations().ownerLeaseFencingMarginMs(1);
      const objects = builder
        .addRouteMesh(mesh)
        .listen('tcp://127.0.0.1:0')
        .routingId(`${node}-${mesh}`)
        .objects();
      if (node === 'a') objects.server();
      else objects.client();
      builder
        .addStreamNode('login')
        .bind('ws://127.0.0.1:*')
        .registerSession(SessionFactory)
        .setAuthenticationAdmission('Login', 8)
        .setAdmissionQueueEnabled(true)
        .setAdmissionQueueKey(key)
        .setAdmissionGlobalLimit(1);
      class AppModule {}
      Module({
        imports: [nest.ZLinkModule.forRoot(builder.build())],
        providers: [SessionFactory]
      })(AppModule);
      const app = await NestFactory.createApplicationContext(AppModule, {
        logger: false,
        abortOnError: false
      });
      apps.push(app);
      const runtime = app.get(nest.ZLINK_FRAMEWORK_RUNTIME);
      await finishFixedRidHostStartup(
        runtime,
        clock,
        framework.ZLinkFrameworkRuntimeState.Preparing
      );
      assert.equal(runtime.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
      return runtime.getListenerStatus('stream', 'login').endpoint;
    }

    async function connect(endpoint) {
      const client = connector.zlinkStreamConnectorFactory.create({
        endpoint,
        heartbeat: { enabled: false },
        dispatchMode: connector.ZlinkStreamDispatchMode.Immediate
      });
      clients.push(client);
      await client.connect();
      return client;
    }

    const login = (client, id) => client.request({ id }).packetName('Login').submit(abort.signal);
    try {
      const a = await start('a');
      const b = await start('b');
      const held = await connect(a);
      const other = await connect(b);
      const third = await connect(b);
      const first = login(held, 'held');
      const entered = await Promise.race([firstEntered.promise, first]);
      assert.equal(entered, undefined, 'first request must enter and block inside the handler');
      const otherPosition = deferred();
      const thirdPosition = deferred();
      other.onAdmissionPosition(otherPosition.resolve);
      third.onAdmissionPosition(thirdPosition.resolve);
      const second = login(other, 'other');
      const thirdResult = login(third, 'third');
      const rest = Promise.all([second, thirdResult]);
      const waiting = await Promise.race([
        Promise.all([otherPosition.promise, thirdPosition.promise, waitingReport.promise]),
        prematurePermitRequest.promise,
        rest
      ]);
      assert.ok(
        Array.isArray(waiting),
        'a waiting node must have no ticket to submit for a permit'
      );
      const [otherRank, thirdRank, issued] = waiting;
      assert.deepEqual([otherRank, thirdRank].sort(), [1, 2]);
      assert.deepEqual(issued, [undefined, undefined]);
      assert.deepEqual(paths, [{ id: 'held', node: 'a' }]);
      firstRelease.resolve();
      assert.deepEqual(await first, { id: 'held' });
      assert.deepEqual(await rest, [{ id: 'other' }, { id: 'third' }]);
      assert.equal(maximum, 1);
      assert.deepEqual(
        paths.sort((left, right) => left.id.localeCompare(right.id)),
        [
          { id: 'held', node: 'a' },
          { id: 'other', node: 'b' },
          { id: 'third', node: 'b' }
        ]
      );
    } finally {
      abort.abort();
      firstRelease.resolve();
      for (const client of clients) await client.close();
      for (const app of apps.reverse()) await app.close();
    }
  }
);
