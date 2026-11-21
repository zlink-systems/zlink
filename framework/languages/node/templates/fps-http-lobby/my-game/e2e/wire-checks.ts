import 'reflect-metadata';
import '../.zlink-framework-json-schemas.cjs';
import assert from 'node:assert/strict';
import { mkdirSync } from 'node:fs';
import { Injectable } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  zlinkFramework,
  zlinkModule,
  ZLinkModule,
  ZLINK_FRAMEWORK_RUNTIME
} from '@zlink-systems/nestjs';
import type {
  ZLinkFrameworkRuntime,
  ZLinkSession,
  ZLinkSessionContext,
  ZLinkSessionFactory
} from '@zlink-systems/framework';
import type { ZlinkStreamEncodedPayload } from '@zlink-systems/stream-connector';
import { MatchPhase } from '../server/shared/constants';
import { Vec3 } from '../server/shared/geometry';
import {
  ActionRejected,
  MaxSequence,
  MoveRequest,
  PlayerView,
  RoomView,
  RoomViewRequest,
  SessionInfoRequest,
  ShotResult
} from '../server/shared/messages';

@Injectable()
class WireSessionFactory implements ZLinkSessionFactory {
  async create(context: ZLinkSessionContext): Promise<ZLinkSession> {
    return {
      context,
      async onDispatch(dispatch): Promise<void> {
        if (dispatch.packetName === RoomViewRequest.name) {
          await context.client.send(new RoomView('before-shot')).submit();
          if (dispatch.canReply) await context.client.reply(new RoomView('after-shot')).submit();
          return;
        }
        await context.client.send(new MoveRequest(new Vec3(1, 0, -2), 7n)).submit();
        await context.client.send(new ShotResult('hit', null, MaxSequence)).submit();
        await context.client
          .send(
            new RoomView(
              'room-x',
              'matchA',
              [new PlayerView('a', new Vec3(48, 0, 18), 100, 4n)],
              MatchPhase.Playing,
              null
            )
          )
          .submit();
        await context.client.send(new ActionRejected('move-budget', 120)).submit();
      }
    };
  }
}

export async function runWireChecks(): Promise<void> {
  const { zlinkStreamConnectorFactory, ZlinkStreamDispatchMode } = await import(
    '@zlink-systems/stream-connector'
  );
  const builder = zlinkFramework();
  builder.addStreamNode('wire-check').bind('ws://127.0.0.1:0').registerSession(WireSessionFactory);
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
  const endpoint = app
    .get<ZLinkFrameworkRuntime>(ZLINK_FRAMEWORK_RUNTIME)
    .getListenerStatus('stream', 'wire-check').endpoint;
  const client = zlinkStreamConnectorFactory.create({
    endpoint,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    reconnect: { enabled: false }
  });
  try {
    const expected: Record<string, string> = {
      MoveRequest: '{"delta":{"x":1,"y":0,"z":-2},"sequence":"7"}',
      ShotResult: '{"outcome":"hit","targetId":null,"sequence":"9007199254740991"}',
      RoomView:
        '{"roomId":"room-x","nodeId":"matchA","players":[{"playerId":"a","position":{"x":48,"y":0,"z":18},"hp":100,"lastSequence":"4"}],"phase":"Playing","winner":null}',
      ActionRejected: '{"reason":"move-budget","retryAfterMs":120}'
    };
    const received: Promise<void>[] = [];
    for (const [name, golden] of Object.entries(expected)) {
      received.push(
        new Promise<void>((resolve, reject) => {
          client.on<ZlinkStreamEncodedPayload>(name, (message) => {
            try {
              assert.deepStrictEqual(
                JSON.parse(new TextDecoder().decode(message.payload.payload)),
                JSON.parse(golden),
                `Wire golden: ${name}`
              );
              resolve();
            } catch (error) {
              reject(error);
            }
          });
        })
      );
    }
    received.push(
      new Promise<void>((resolve, reject) => {
        client.on<RoomView>(RoomView, (message) => {
          try {
            assert.equal(message.payload.players[0].lastSequence, 4n);
            assert.equal(message.payload.phase, MatchPhase.Playing);
            resolve();
          } catch (error) {
            reject(error);
          }
        });
      })
    );
    await client.connect();
    await client.send(new SessionInfoRequest()).submit();
    await Promise.race([
      Promise.all(received),
      new Promise<never>((_, reject) => {
        const timer = setTimeout(() => reject(new Error('Wire golden deadline exceeded')), 15000);
        timer.unref();
      })
    ]);
    console.log('Wire golden: 1/1 passed (4 samples)');
    const { PlayerClient } = await import('./client.mjs');
    mkdirSync('.logs', { recursive: true });
    const player = new PlayerClient(endpoint, '.logs/view-checks.jsonl', 'view-check');
    try {
      await player.connect();
      const view = await player.view();
      assert.equal(view.roomId, 'after-shot', 'Room query selected an unsolicited broadcast.');
      assert(
        player.messages.some(
          (message) =>
            message.name === RoomView.name && (message.payload as RoomView).roomId === 'before-shot'
        ),
        'Room query must still receive unsolicited broadcasts.'
      );
      console.log('Room view correlation: 1/1 passed (broadcast before reply)');
    } finally {
      await player.close();
    }
  } finally {
    await client.close();
    await app.close();
  }
}

void runWireChecks();
