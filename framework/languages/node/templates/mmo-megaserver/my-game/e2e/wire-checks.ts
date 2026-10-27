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
import { Game } from '../server/shared/constants';
import { ActionRejected, MoveRequest, PlayerPosition, RegionView, SessionInfoRequest } from '../server/shared/messages';

@Injectable()
class WireSessionFactory implements ZLinkSessionFactory {
  async create(context: ZLinkSessionContext): Promise<ZLinkSession> {
    return {
      context,
      async onDispatch(): Promise<void> {
        await context.client.send(new MoveRequest(10, Game.maxSafeInteger)).submit();
        await context.client.send(new RegionView('forest-1', 'regionA', [new PlayerPosition('p1', 10, 4n)])).submit();
        await context.client.send(new ActionRejected('move-budget', 120)).submit();
      }
    };
  }
}

export async function runWireChecks(): Promise<void> {
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
      MoveRequest: '{"delta":10,"sequence":"9007199254740991"}',
      RegionView: '{"regionId":"forest-1","nodeId":"regionA","players":[{"playerId":"p1","x":10,"sequence":"4"}]}',
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
        client.on<RegionView>(RegionView, (message) => {
          try {
            assert.equal(message.payload.players[0].sequence, 4n);
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
    console.log('Wire golden: 1/1 passed (3 samples)');
  } finally {
    await client.close();
    await app.close();
  }
}

void runWireChecks();
