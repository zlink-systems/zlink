'use strict';

const fs = require('node:fs');
const assert = require('node:assert/strict');
const { Module, Injectable, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { logs } = require('@opentelemetry/api-logs');
const { LoggerProvider } = require('@opentelemetry/sdk-logs');
const framework = require('../../../packages/framework/dist');
const nestjs = require('../../../packages/nestjs/dist');

const mode = process.argv[2];
const provider = new LoggerProvider({
  processors: [
    {
      onEmit(record) {
        if (process.env.ZLINK_TEST_FLOW_FILE) {
          fs.appendFileSync(
            process.env.ZLINK_TEST_FLOW_FILE,
            JSON.stringify({ body: record.body, attributes: record.attributes }, (_key, value) =>
              typeof value === 'bigint' ? String(value) : value
            ) + '\n'
          );
        }
      },
      forceFlush: async () => {},
      shutdown: async () => {}
    }
  ]
});
logs.setGlobalLoggerProvider(provider);

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}
const targetEntered = deferred();
const targetRelease = deferred();
const sourceCompleted = deferred();
const sourceClosed = deferred();
const targetScopeDisposed = deferred();
const events = [];
let requestTerminals = 0;
let timerCancelled;
let targetId;
class Hold {}
class TargetSpot {
  async onClosing() {
    assert.equal(events.at(-1), 'target-ended');
    events.push('target-closing');
  }
}
class SourceSpot {
  async onInitialize() {
    this.timer = await this.context.addTimer('request-once', 10, RequestTimer);
  }
  async onClosing() {
    assert.equal(requestTerminals, 1);
    sourceClosed.resolve();
  }
}
class HoldHandler {
  async handle() {
    events.push('target-started');
    console.log('target-handler-entered');
    targetEntered.resolve();
    await targetRelease.promise;
    events.push('target-ended');
    console.log('target-handler-released');
    return { ok: true };
  }

  dispose() {
    assert.equal(events.at(-1), 'target-closing');
    events.push('target-scope-disposed');
    targetScopeDisposed.resolve();
  }
}
class RequestTimer {
  async handle(spot) {
    timerCancelled = spot.timer.cancel();
    try {
      if (mode === 'seal') {
        await spot.context
          .runIoWorker(async () => {
            console.log('target-handler-entered');
            targetEntered.resolve();
            await targetRelease.promise;
          })
          .yield();
      } else {
        const request = spot.context.outbound.requestToSpot(targetId, new Hold());
        await (mode === 'held-yield' ? request.yield() : request.submit());
      }
    } catch (error) {
      console.error(error);
      throw error;
    } finally {
      requestTerminals++;
      console.log('source-handler-completed');
      sourceCompleted.resolve();
    }
  }
}
Injectable({ scope: Scope.TRANSIENT })(HoldHandler);
for (const type of [TargetSpot, SourceSpot]) Injectable({ scope: Scope.TRANSIENT })(type);
nestjs.zlinkSpotPacketHandler({ packetName: 'Hold', spot: () => TargetSpot })(HoldHandler);

async function main() {
  const builder = nestjs.zlinkFramework().options({ locations: { useInMemoryStores: true } });
  builder.configureDispatch().messageFlow('normal');
  builder
    .addRouteMesh('shutdown-execution')
    .listen('tcp://127.0.0.1:0')
    .routingId('shutdown-execution-node')
    .objects()
    .server()
    .addSpotFactory('target', TargetSpot, (factory) => factory.disableRelocation())
    .addSpotFactory('source', SourceSpot, (factory) => factory.disableRelocation());
  class AppModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRoot(builder.build())],
    providers: [TargetSpot, SourceSpot, HoldHandler, RequestTimer]
  })(AppModule);
  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
  const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME);
  const spots = app.get(nestjs.ZLINK_SPOT_MANAGER);
  if (mode !== 'seal')
    targetId = (await spots.create('target').inMesh('shutdown-execution').submit()).spot.spotId;
  await spots.create('source').inMesh('shutdown-execution').submit();
  await targetEntered.promise;
  if (mode === 'completed') {
    targetRelease.resolve();
    await sourceCompleted.promise;
    await timerCancelled;
  }
  console.log('shutdown-started');
  const result = await (mode === 'completed'
    ? runtime.shutdown()
    : runtime.shutdown({ deadlineMs: 50 }));
  console.log('shutdown-returned:' + JSON.stringify(result));
  if (mode !== 'completed') {
    if (
      result.outcome !== framework.ZLinkFrameworkTerminationOutcome.ForceStopped ||
      result.reason !== framework.ZLinkFrameworkTerminationReason.DeadlineExceeded
    ) {
      throw new Error('A held handler must report ForceStopped/DeadlineExceeded.');
    }
  } else if (
    result.outcome !== framework.ZLinkFrameworkTerminationOutcome.Stopped ||
    result.reason !== framework.ZLinkFrameworkTerminationReason.None
  ) {
    throw new Error('A completed handler must report Stopped/None.');
  }
  if (mode !== 'completed') {
    assert.deepEqual(events, mode === 'seal' ? [] : ['target-started']);
    targetRelease.resolve();
    targetRelease.resolve();
  }
  if (mode !== 'seal') await targetScopeDisposed.promise;
  await sourceClosed.promise;
  await timerCancelled;
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(
    events,
    mode === 'seal'
      ? []
      : ['target-started', 'target-ended', 'target-closing', 'target-scope-disposed']
  );
  assert.equal(requestTerminals, 1, 'late reply must not complete the request twice');
  console.log('scope-and-late-completion-verified');
  await app.close();
  await provider.shutdown();
}
main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
