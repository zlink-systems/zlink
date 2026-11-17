'use strict';

require('reflect-metadata');
const assert = require('node:assert/strict');
const { availableParallelism } = require('node:os');
const { Module, Injectable, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../../packages/nestjs/dist');

assert.equal(Math.max(2, availableParallelism()), 2);
console.log('application-workers=2');
let arrived = 0;
let release;
const both = new Promise(resolve => { release = resolve; });
class Outer { constructor(id) { this.id = id; } }
class Inner { constructor(id) { this.id = id; } }
class Room { configure() {} }
Injectable({ scope: Scope.TRANSIENT })(Room);
class OuterHandler {
  async handle(spot, request) {
    console.log(`A-enter:${request.id}`);
    if (++arrived === 2) release();
    await both;
    return await spot.context.outbound.requestToSpot('b', new Inner(request.id))
      .timeout(2000).submit();
  }
}
class InnerHandler {
  handle(_spot, request) {
    console.log(`B-enter:${request.id}`);
    return { id: request.id, ok: true };
  }
}
nestjs.zlinkSpotPacketHandler({ spot: () => Room, packetName: 'Outer' })(OuterHandler);
nestjs.zlinkSpotPacketHandler({ spot: () => Room, packetName: 'Inner' })(InnerHandler);

async function main() {
  const builder = nestjs.zlinkFramework().options({ locations: { useInMemoryStores: true } });
  builder.configureDispatch().messageFlow('normal');
  builder.addRouteMesh('suspended-workers').listen('tcp://127.0.0.1:0').objects().server()
    .addSpotFactory('room', Room, factory => factory.disableRelocation());
  class App {}
  Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())],
    providers: [Room, OuterHandler, InnerHandler] })(App);
  const app = await NestFactory.createApplicationContext(App, { logger: false, abortOnError: false });
  try {
    const manager = app.get(nestjs.ZLINK_SPOT_MANAGER);
    for (const id of ['b', 'a0', 'a1']) {
      await manager.getOrCreate(id, 'room').inMesh('suspended-workers').submit();
    }
    const outbound = app.get(nestjs.ZLINK_SPOT_OUTBOUND);
    const replies = await Promise.allSettled([0, 1].map(id =>
      outbound.requestToSpot(`a${id}`, new Outer(id)).timeout(2000).submit()));
    console.log(JSON.stringify(replies));
    assert.deepEqual(replies, [0, 1].map(id => ({
      status: 'fulfilled', value: { id, ok: true }
    })));
    console.log('two-responses');
  } finally {
    await app.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
