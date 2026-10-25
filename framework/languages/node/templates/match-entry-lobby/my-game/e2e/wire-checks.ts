import 'reflect-metadata';
import '../.zlink-framework-json-schemas.cjs';
import assert from 'node:assert/strict';
import { createServer } from 'node:net';
import { Injectable } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { zlinkFramework, zlinkModule, ZLinkModule } from '@zlink-systems/nestjs';
import type { ZLinkSession, ZLinkSessionContext, ZLinkSessionFactory } from '@zlink-systems/framework';
import type { ZlinkStreamEncodedPayload } from '@zlink-systems/stream-connector';
import {
  ActionRejected,
  MatchEnded,
  MoveRequest,
  MoveResult,
  PlayerView,
  QueueRequest,
  RoomView,
  SessionInfoRequest,
  ShootRequest,
  ShotResult,
  signedInt32,
  toRetryAfterMs,
  type Int32
} from '../server/shared/messages.js';

const bigSequence = 9_007_199_254_740_993n;
const expected: Record<string, string> = {
  QueueRequest: '{"playerId":"p1","mode":"duel"}',
  MoveRequest: '{"dx":1,"dy":0,"sequence":"7"}',
  ShootRequest: '{"targetId":"p2","sequence":"9007199254740993"}',
  MoveResult: '{"x":41,"y":10,"sequence":"7"}',
  ShotResult: '{"outcome":"hit","targetId":"p2","sequence":"9007199254740993"}',
  RoomView:
    '{"roomId":"room-x","nodeId":"matchA","phase":"Playing","winner":null,"players":[{"playerId":"p1","x":41,"y":10,"hp":75,"lastSequence":"9007199254740993"}]}',
  ActionRejected: '{"reason":"move-budget","retryAfterMs":120}',
  MatchEnded: '{"winner":"p1"}'
};

@Injectable()
class WireSessionFactory implements ZLinkSessionFactory {
  async create(context: ZLinkSessionContext): Promise<ZLinkSession> {
    return {
      context,
      async onDispatch(): Promise<void> {
        await context.client.send(new QueueRequest('p1', 'duel')).submit();
        await context.client.send(new MoveRequest(1, 0, 7n)).submit();
        await context.client.send(new ShootRequest('p2', bigSequence)).submit();
        await context.client.send(new MoveResult(41, 10, 7n)).submit();
        await context.client.send(new ShotResult('hit', 'p2', bigSequence)).submit();
        await context.client
          .send(
            new RoomView('room-x', 'matchA', 'Playing', null, [
              new PlayerView('p1', 41, 10, 75, bigSequence)
            ])
          )
          .submit();
        await context.client.send(new ActionRejected('move-budget', signedInt32(120))).submit();
        await context.client.send(new MatchEnded('p1')).submit();
      }
    };
  }
}

export async function runWireChecks(): Promise<void> {
  const retryMax = signedInt32(2_147_483_647);
  assert.equal(toRetryAfterMs(Number.POSITIVE_INFINITY), retryMax);
  assert.equal(toRetryAfterMs(Number.NEGATIVE_INFINITY), 0);
  assert.throws(() => signedInt32(2_147_483_648), RangeError);
  assert.throws(() => signedInt32(1.5), RangeError);
  assert.throws(() => new ActionRejected('invalid', 2_147_483_648 as Int32), RangeError);
  const { zlinkStreamConnectorFactory, ZlinkStreamDispatchMode } = await import(
    '@zlink-systems/stream-connector'
  );
  const reservation = createServer();
  await new Promise<void>((resolve) => reservation.listen(0, '127.0.0.1', resolve));
  const address = reservation.address();
  assert(address && typeof address === 'object');
  const endpoint = `ws://127.0.0.1:${address.port}`;
  await new Promise<void>((resolve, reject) =>
    reservation.close((error) => (error ? reject(error) : resolve()))
  );
  const builder = zlinkFramework();
  builder.addStreamNode('wire-check').bind(endpoint).registerSession(WireSessionFactory);
  class WireHost {}
  zlinkModule({
    providerDiscovery: [],
    imports: [ZLinkModule.forRoot(builder.build())],
    providers: [WireSessionFactory]
  })(WireHost);
  const app = await NestFactory.createApplicationContext(WireHost, {
    logger: ['error'],
    abortOnError: false
  });
  const client = zlinkStreamConnectorFactory.create({
    endpoint,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    reconnect: { enabled: false }
  });
  const received = Object.keys(expected).map(
    (name) =>
      new Promise<void>((resolve, reject) => {
        client.on<ZlinkStreamEncodedPayload>(name, (message) => {
          try {
            assert.deepEqual(
              JSON.parse(new TextDecoder().decode(message.payload.payload)),
              JSON.parse(expected[name]),
              `Wire golden: ${name}`
            );
            resolve();
          } catch (error) {
            reject(error);
          }
        });
      })
  );
  received.push(
    new Promise<void>((resolve, reject) => {
      client.on<MoveRequest>(MoveRequest, (message) => {
        try {
          assert.equal(message.payload.sequence, 7n);
          assert.equal(typeof message.payload.sequence, 'bigint');
          resolve();
        } catch (error) {
          reject(error);
        }
      });
    })
  );
  received.push(
    new Promise<void>((resolve, reject) => {
      client.on<RoomView>(RoomView, (message) => {
        try {
          assert.equal(message.payload.players[0].lastSequence, bigSequence);
          assert.equal(typeof message.payload.players[0].lastSequence, 'bigint');
          resolve();
        } catch (error) {
          reject(error);
        }
      });
    })
  );
  try {
    await client.connect();
    await client.send(new SessionInfoRequest()).submit();
    await Promise.race([
      Promise.all(received),
      new Promise<never>((_, reject) => {
        const timer = setTimeout(() => reject(new Error('Wire golden deadline exceeded')), 15000);
        timer.unref();
      })
    ]);
    console.log(`Wire golden: ${Object.keys(expected).length}/${Object.keys(expected).length} passed`);
  } finally {
    await client.close();
    await app.close();
  }
}

void runWireChecks();
