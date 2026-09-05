const assert = require('node:assert/strict');
const { createHook } = require('node:async_hooks');
const path = require('node:path');
const os = require('node:os');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../../packages/framework/dist');
const nestjs = require('../../../packages/nestjs/dist');
const { RoutingId } = require('@zlink-systems/zlink');

const mode = process.argv[2] ?? 'normal';
const resources = new Map();
const tracking = createHook({
  init(id, type, _trigger, resource) {
    const stack = new Error().stack;
    if (/packages\/(framework|nestjs)\/dist\/|node_modules\/@zlink-systems\/zlink\//.test(stack)) {
      resources.set(id, { type, resource });
    }
  },
  destroy(id) { resources.delete(id); }
}).enable();
const initialized = [];
const closed = [];
class Tick { async handle() {} }
class TimedSpot {
  async onInitialize() {
    await this.context.addTimer('shutdown-regression', 1000, Tick);
    initialized.push(this.constructor.name);
  }
  async onClosing(context, cleanupSignal) {
    closed.push([this.constructor.name, context.reason]);
    if (this instanceof UserSpot && mode === 'failure') throw new Error('closing regression');
    if (this instanceof UserSpot && mode === 'deadline') {
      await new Promise((resolve, reject) => {
        cleanupSignal.addEventListener('abort', () => reject(cleanupSignal.reason), { once: true });
      });
    }
  }
}
class EntrySpot extends TimedSpot {}
class UserSpot extends TimedSpot {}
class InstanceSpot extends TimedSpot {}
for (const type of [EntrySpot, UserSpot, InstanceSpot]) Injectable({ scope: Scope.TRANSIENT })(type);

async function main() {
  const builder = nestjs.zlinkFramework().options({ locations: { useInMemoryStores: true } });
  builder.addRouteMesh('shutdown-test')
    .listen(`ipc://${path.join(os.tmpdir(), `nest-shutdown-${process.pid}.sock`)}`)
    .routingId('shutdown-node')
    .objects().server()
    .addEntrySpot(EntrySpot)
    .addSpotFactory('user', UserSpot, factory => factory.disableRelocation())
    .addInstanceSpotFactory('instance', InstanceSpot, factory => factory.disableRelocation());
  class AppModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRoot(builder.build())],
    providers: [EntrySpot, UserSpot, InstanceSpot, Tick]
  })(AppModule);
  const app = await NestFactory.createApplicationContext(AppModule, { logger: false, abortOnError: false });
  const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME);
  await runtime.spotNodeRuntime.ensureEntryActivation('shutdown-test');
  await app.get(nestjs.ZLINK_SPOT_MANAGER).create('user').inMesh('shutdown-test').submit();
  // Isolate host teardown from remote Instance placement and admission.
  await runtime.spotManager.materializeInstance(
    'shutdown-test', 'instance', RoutingId.from('shutdown-instance')
  );
  assert.deepEqual(initialized.sort(), ['EntrySpot', 'InstanceSpot', 'UserSpot']);
  const existing = mode === 'normal' ? undefined : runtime.shutdown({ deadlineMs: mode === 'deadline' ? 50 : 5000 });
  await app.close();
  const terminal = await runtime.shutdown();
  if (existing) assert.equal(await existing, terminal);
  assert.equal(terminal.outcome, mode === 'normal' || mode === 'shared'
    ? framework.ZLinkFrameworkTerminationOutcome.Stopped : framework.ZLinkFrameworkTerminationOutcome.ForceStopped);
  assert.equal(terminal.reason, mode === 'failure'
    ? framework.ZLinkFrameworkTerminationReason.TeardownFailed
    : mode === 'deadline' ? framework.ZLinkFrameworkTerminationReason.DeadlineExceeded
      : framework.ZLinkFrameworkTerminationReason.None);
  if (mode !== 'deadline') {
    assert.deepEqual(closed.map(([name]) => name).sort(), initialized);
    assert.ok(closed.every(([, reason]) => reason === framework.ZLinkSpotCloseReason.HostShutdown));
  }
  await new Promise(resolve => setImmediate(resolve));
  await new Promise(resolve => setImmediate(resolve));
  const owned = new Set([...resources.values()].map(entry => entry.resource));
  const handles = process._getActiveHandles().filter(handle => owned.has(handle));
  const timers = [...resources.values()].filter(({ type, resource }) =>
    type === 'Timeout' && resource.hasRef() && !resource._destroyed);
  assert.deepEqual(handles, [], 'framework-owned active handles after app.close');
  assert.deepEqual(timers, [], 'framework-owned referenced timers after app.close');
  tracking.disable();
  console.log(JSON.stringify({ mode, terminal, closed, handles: handles.length, timers: timers.length }));
}
main().catch(error => { console.error(error); process.exitCode = 1; });
