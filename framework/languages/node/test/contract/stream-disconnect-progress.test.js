'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const test = require('node:test');
require('./helpers/telemetry-log-capture');
const binding = require('@zlink-systems/zlink');
const originalCreate = binding.createStreamSocket;
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');
const connector = require('../../packages/stream-connector/dist');
// Existing test store implements the public location-store SPI; no runtime
// object, Actor state, callback queue or socket is obtained through this import.
const { ZLinkInMemoryProviderLocationStore } = require('../../packages/framework/dist/internal');

const STAGE_MS = 5000;
const PROGRESS_MS = 1000;
const deferred = () => {
  let resolve;
  const promise = new Promise((done) => { resolve = done; });
  return { promise, resolve };
};
async function bounded(promise, name, timeoutMs = STAGE_MS) {
  let timer;
  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error(`Prerequisite failed: ${name}`)), timeoutMs);
      })
    ]);
  } finally {
    clearTimeout(timer);
  }
}

test('physical disconnect cleans bound Actor while Session handler awaits',
  { timeout: 20000 }, async (t) => {
    const entered = deferred();
    const release = deferred();
    const handlerDone = deferred();
    const disconnected = deferred();
    const physical = deferred();
    const actorDisconnected = deferred();
    const events = [];
    let app;
    let client;
    let actorRef;
    let sessionRid;
    let handlerHeld = false;
    let sessionCallbacks = 0;
    let actorCallbacks = 0;
    let streamSockets = 0;
    let monitors = 0;
    let physicalSeen = false;
    let beforeRelease = false;
    let primaryError;
    const cleanupErrors = [];
    const restorations = [];

    // Instrument only public methods on the one existing real STREAM socket
    // and its one existing monitor. Every native event/result is returned intact.
    // The public WebSocket connector creates no binding STREAM socket of its own.
    t.mock.method(binding, 'createStreamSocket', function (...args) {
      const socket = originalCreate.apply(this, args);
      streamSockets++;
      const originalOpen = socket.monitorOpen;
      const openDescriptor = Object.getOwnPropertyDescriptor(socket, 'monitorOpen');
      socket.monitorOpen = function (...openArgs) {
        const monitor = originalOpen.apply(this, openArgs);
        monitors++;
        const originalRecv = monitor.recv;
        const recvDescriptor = Object.getOwnPropertyDescriptor(monitor, 'recv');
        monitor.recv = function (...recvArgs) {
          const event = originalRecv.apply(this, recvArgs);
          if (event !== null && event.event === binding.MonitorEventType.Disconnected &&
              (event.routingId == null || sessionRid === undefined ||
               event.routingId.toHex() === sessionRid)) {
            physicalSeen = true;
            events.push({ stage: 'native_disconnected', rid: event.routingId?.toHex(),
              localAddr: event.localAddr, remoteAddr: event.remoteAddr });
            physical.resolve();
          }
          return event;
        };
        restorations.push(() => {
          if (recvDescriptor) Object.defineProperty(monitor, 'recv', recvDescriptor);
          else delete monitor.recv;
        });
        return monitor;
      };
      restorations.push(() => {
        if (openDescriptor) Object.defineProperty(socket, 'monitorOpen', openDescriptor);
        else delete socket.monitorOpen;
      });
      return socket;
    });

    class Player {
      constructor(context) { this.context = context; }
    }
    class Factory {
      async create(context) { return new Player(context); }
    }
    class Entry {
      constructor(context) { this.context = context; }
      configure() {}
      async onDisconnectActor() {
        actorCallbacks++;
        events.push({ stage: 'actor_disconnected', handlerHeld });
        actorDisconnected.resolve();
      }
    }
    class SessionFactory {
      async create(context) {
        sessionRid = context.routingId === undefined ? undefined : String(context.routingId);
        return {
          context,
          async onDispatch(dispatch) {
            if (dispatch.packetName === 'Bind') {
              await context.actors.bindOrGet(actorRef);
              await context.client.reply({ bound: true }).submit();
              return;
            }
            assert.equal(dispatch.packetName, 'Hold');
            handlerHeld = true;
            events.push({ stage: 'session_handler_entered' });
            entered.resolve();
            try { await release.promise; }
            finally {
              handlerHeld = false;
              events.push({ stage: 'session_handler_released' });
              handlerDone.resolve();
            }
          },
          async onDisconnected() {
            sessionCallbacks++;
            assert.equal(handlerHeld, false, 'Session callbacks remain serial');
            events.push({ stage: 'session_disconnected' });
            disconnected.resolve();
          }
        };
      }
    }

    try {
      const builder = nestjs.zlinkFramework()
        .addLocationStore(new ZLinkInMemoryProviderLocationStore());
      builder.configureDispatch().messageFlow('normal');
      builder.addRouteMesh('disconnect-progress').listen('tcp://127.0.0.1:0')
        .objects().server().addEntrySpot(Entry, 'player')
        .addActorFactory('player', Factory, (f) => f.disableRelocation());
      builder.addStreamNode('clients').bind('ws://127.0.0.1:*')
        .enableActorDispatch().registerSession(SessionFactory);
      class AppModule {}
      Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())],
        providers: [Factory, SessionFactory] })(AppModule);
      app = await bounded(NestFactory.createApplicationContext(AppModule,
        { logger: false, abortOnError: false }), 'host startup');
      const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER);
      await bounded(actors.getOrCreate('disconnect-progress-player', 'player')
        .inMesh('disconnect-progress').submit(), 'public Actor creation');
      actorRef = await bounded(actors.find('disconnect-progress-player'), 'public Actor lookup');
      assert.ok(actorRef);
      client = connector.zlinkStreamConnectorFactory.create({
        endpoint: app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME)
          .getListenerStatus('stream', 'clients').endpoint,
        dispatchMode: connector.ZlinkStreamDispatchMode.Immediate
      });
      await bounded(client.connect(), 'public WebSocket connect');
      assert.deepEqual(await bounded(client.request({}).packetName('Bind').submit(),
        'public Actor binding and reply'), { bound: true });
      await bounded(client.send({}).packetName('Hold').submit(), 'held request admission');
      await bounded(entered.promise, 'Session handler entered');
      await bounded(client.close(), 'public WebSocket client close');
      await bounded(physical.promise, 'server existing monitor observed physical disconnect');
      if (process.env.ZLINK_DISCONNECT_PROGRESS_RESULT) {
        fs.writeFileSync(`${process.env.ZLINK_DISCONNECT_PROGRESS_RESULT}.maps`,
          fs.readFileSync('/proc/self/maps'));
      }
      assert.equal(streamSockets, 1, 'one server STREAM socket');
      assert.equal(monitors, 1, 'one unchanged existing server monitor');
      assert.equal(physicalSeen, true);
      assert.equal(handlerHeld, true);
      assert.equal(sessionCallbacks, 0, 'Session terminal callback waits its handler');
      let timer;
      try {
        beforeRelease = await Promise.race([
          actorDisconnected.promise.then(() => true),
          new Promise((resolve) => { timer = setTimeout(() => resolve(false), PROGRESS_MS); })
        ]);
      } finally { clearTimeout(timer); }
      assert.equal(handlerHeld, true, 'test owns the unresolved application wait');
      assert.equal(sessionCallbacks, 0);
      release.resolve();
      await bounded(handlerDone.promise, 'released Session handler terminal');
      await bounded(actorDisconnected.promise, 'Actor notification after release control');
      await bounded(disconnected.promise, 'serialized Session disconnected callback');
      assert.equal(actorCallbacks, 1, 'exactly one real public Spot notification');
      assert.equal(sessionCallbacks, 1);
      assert.equal(beforeRelease, true,
        'Automatic Actor disconnect must progress before held Session handler is released');
    } catch (error) {
      primaryError = error;
    } finally {
      release.resolve();
      for (const [name, work] of [['client close', () => client?.close()],
        ['host close', () => app?.close()]]) {
        try { await bounded(Promise.resolve().then(work), name); }
        catch (error) { cleanupErrors.push(error); }
      }
      for (const restore of restorations.reverse()) restore();
      t.mock.restoreAll();
      if (process.env.ZLINK_DISCONNECT_PROGRESS_RESULT) {
        fs.writeFileSync(process.env.ZLINK_DISCONNECT_PROGRESS_RESULT,
          JSON.stringify({ pid: process.pid, physicalSeen, beforeRelease, actorCallbacks,
            sessionCallbacks, streamSockets, monitors, events,
            primaryError: primaryError?.stack,
            cleanupErrors: cleanupErrors.map((error) => error.stack) }, null, 2));
      }
    }
    if (primaryError && cleanupErrors.length) {
      throw new AggregateError([primaryError, ...cleanupErrors], 'Causal test and cleanup failed');
    }
    if (primaryError) throw primaryError;
    if (cleanupErrors.length) throw new AggregateError(cleanupErrors, 'Cleanup failed');
  });
