import 'reflect-metadata';
import '../.zlink-framework-json-schemas.cjs';
import assert from 'node:assert/strict';
import { createServer } from 'node:net';
import { Injectable } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { zlinkFramework, zlinkModule, ZLinkModule } from '@zlink-systems/nestjs';
import type {
  ZLinkSession,
  ZLinkSessionContext,
  ZLinkSessionFactory
} from '@zlink-systems/framework';
import type { ZlinkStreamEncodedPayload } from '@zlink-systems/stream-connector';
import { Limits, PacketNames } from '../server/shared/constants';
import {
  ActionRejected,
  ChannelView,
  GameRules,
  PlayerView,
  SessionInfoRequest
} from '../server/shared/messages';

@Injectable()
class WireSessionFactory implements ZLinkSessionFactory {
  async create(context: ZLinkSessionContext): Promise<ZLinkSession> {
    return {
      context,
      async onDispatch(): Promise<void> {
        await context.client
          .send(new PlayerView('wire', GameRules.maxInteger, 'wire-check', GameRules.maxInteger))
          .submit();
        await context.client.send(new ChannelView(GameRules.maxInteger, ['wire-player'])).submit();
        await context.client.send(new ActionRejected('earn-too-soon', 1)).submit();
      }
    };
  }
}

async function runWireChecks(): Promise<void> {
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
  try {
    const expected: Record<string, string> = {
      [PacketNames.playerView]:
        '{"worldId":"wire","channel":"9007199254740991","nodeId":"wire-check","gold":"9007199254740991"}',
      [PacketNames.channelView]:
        '{"channelId":"9007199254740991","playerIds":["wire-player"]}',
      [PacketNames.actionRejected]: '{"reason":"earn-too-soon","retryAfterMs":1}'
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
        client.on<PlayerView>(PlayerView, (message) => {
          try {
            assert.equal(message.payload.channel, GameRules.maxInteger);
            assert.equal(message.payload.gold, GameRules.maxInteger);
            resolve();
          } catch (error) {
            reject(error);
          }
        });
      })
    );
    received.push(
      new Promise<void>((resolve, reject) => {
        client.on<ChannelView>(ChannelView, (message) => {
          try {
            assert.equal(message.payload.channelId, GameRules.maxInteger);
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
        const timer = setTimeout(
          () => reject(new Error('Wire golden deadline exceeded')),
          Limits.messageMs
        );
        timer.unref();
      })
    ]);
    console.log('Wire golden: 1/1 passed (3 samples)');
  } finally {
    await client.close();
    await app.close();
  }
}

runWireChecks().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
