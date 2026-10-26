'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { appendFileSync } = require('node:fs');
const { logs } = require('@opentelemetry/api-logs');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const nestjs = require('../../packages/nestjs/dist');

if (process.env.ZLINK_TEST_FLOW_FILE)
  logs.setGlobalLoggerProvider({
    getLogger: () => ({
      enabled: () => true,
      emit: (record) =>
        appendFileSync(
          process.env.ZLINK_TEST_FLOW_FILE,
          JSON.stringify(record.attributes, (_key, value) =>
            typeof value === 'bigint' ? String(value) : value
          ) + '\n'
        )
    })
  });

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

// Public blob provider used by cold Instance activation, independent of Redis.
class MemoryRelocationStore {
  blobs = new Map();
  async read(reference, signal) {
    signal?.throwIfAborted();
    const storeNow = new Date();
    const blob = this.blobs.get(reference.value);
    if (!blob || blob.expiresAt <= storeNow) return { kind: 'missing', storeNow };
    return { kind: 'found', ...blob, bytes: Buffer.from(blob.bytes), storeNow };
  }
  async put(reference, payload, retentionMs, signal) {
    const current = await this.read(reference, signal);
    const storeNow = new Date();
    if (current.kind === 'found')
      return Buffer.from(payload).equals(current.bytes)
        ? { kind: 'alreadyStored', expiresAt: current.expiresAt, storeNow }
        : { kind: 'conflict', storeNow };
    const expiresAt = new Date(storeNow.getTime() + retentionMs);
    this.blobs.set(reference.value, { bytes: Buffer.from(payload), expiresAt });
    return { kind: 'stored', expiresAt, storeNow };
  }
  async renew(reference, retentionMs, signal) {
    const current = await this.read(reference, signal);
    if (current.kind === 'missing') return current;
    const storeNow = new Date();
    const expiresAt = new Date(storeNow.getTime() + retentionMs);
    this.blobs.set(reference.value, { bytes: current.bytes, expiresAt });
    return { kind: 'renewed', expiresAt, storeNow };
  }
  async delete(reference, signal) {
    signal?.throwIfAborted();
    this.blobs.delete(reference.value);
  }
}

test('Instance timer request rejected during app close completes once with ShuttingDown', async () => {
  const entered = deferred();
  const release = deferred();
  const terminal = deferred();
  let terminals = 0;
  let targetCalls = 0;
  class Ping {}
  class ProbeSpot {
    async onInitialize() {
      if (this.context.spotId === 'source')
        this.timer = await this.context.addTimer('request-once', 10, RequestTimer);
    }
  }
  class PingHandler {
    handle(spot) {
      if (spot.context.spotId === 'target') targetCalls++;
      return { ok: true };
    }
  }
  class RequestTimer {
    async handle(spot) {
      void spot.timer.cancel();
      entered.resolve();
      await release.promise;
      try {
        await spot.context.outbound
          .requestToSpot('target', new Ping())
          .instanceSpot('probe')
          .yield();
        terminal.resolve(undefined);
      } catch (error) {
        terminal.resolve(error);
      } finally {
        terminals++;
      }
    }
  }
  Injectable({ scope: Scope.TRANSIENT })(ProbeSpot);
  Injectable()(PingHandler);
  Injectable()(RequestTimer);
  nestjs.zlinkSpotPacketHandler({ spot: () => ProbeSpot, packetName: 'Ping' })(PingHandler);
  const builder = nestjs.zlinkFramework().options({ locations: { useInMemoryStores: true } });
  builder.addRelocationStore(new MemoryRelocationStore());
  builder.configureDispatch().messageFlow('detailed').traceSampleRate(1);
  builder
    .addRouteMesh('shutdown-rejection')
    .listen('tcp://127.0.0.1:0')
    .routingId('shutdown-rejection-node')
    .setAdvertiseHost('127.0.0.1')
    .objects()
    .server()
    .addInstanceSpotFactory('probe', ProbeSpot, (factory) => factory.disableRelocation());
  class AppModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRoot(builder.build())],
    providers: [ProbeSpot, PingHandler, RequestTimer]
  })(AppModule);
  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
  const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME);
  try {
    const outbound = app.get(nestjs.ZLINK_SPOT_OUTBOUND);
    await outbound.requestToSpot('target', new Ping()).instanceSpot('probe').submit();
    await outbound.requestToSpot('target', new Ping()).instanceSpot('probe').submit();
    await outbound.requestToSpot('source', new Ping()).instanceSpot('probe').submit();
    await entered.promise;
    const started = Date.now();
    const shutdown = runtime.shutdown();
    release.resolve();
    const closing = app.close();
    const error = await terminal.promise;
    const terminalElapsedMs = Date.now() - started;
    await closing;
    await shutdown;
    console.log(
      JSON.stringify({
        elapsedMs: Date.now() - started,
        terminalElapsedMs,
        termination: runtime.status.terminationResult,
        error: { name: error?.name, kind: error?.kind, message: error?.message },
        terminals
      })
    );
    assert(error instanceof framework.ZLinkFrameworkException);
    assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.ShuttingDown);
    assert(
      terminalElapsedMs < 5000,
      'admission rejection must complete before the shutdown deadline'
    );
    assert.equal(terminals, 1);
    assert.equal(targetCalls, 2, 'sealed ingress must not run the target handler');
    assert.equal(
      runtime.status.terminationResult.outcome,
      framework.ZLinkFrameworkTerminationOutcome.Stopped
    );
    assert.equal(
      runtime.status.terminationResult.reason,
      framework.ZLinkFrameworkTerminationReason.None
    );
    assert(
      Date.now() - started < 5000,
      'app close must finish before its default shutdown deadline'
    );
  } finally {
    release.resolve();
    await app.close();
  }
});
