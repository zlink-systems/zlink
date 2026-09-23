import 'reflect-metadata';
import * as http from 'node:http';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLINK_ROUTE_CLIENT, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import type { ZLinkRouteClient } from '@zlink-systems/framework';
import { Greeting, Hello } from '../Shared/contracts';

@Module({
  imports: [
    ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = zlinkFramework();
        // This process also opens and advertises its own loopback endpoint.
        const mesh = builder
          .addRouteMesh('services')
          .listen('tcp://127.0.0.1:7102')
          .setAdvertiseHost('127.0.0.1');
        // This side only calls; it does not handle "greeting".
        mesh.channel('greeting').client();
        // Manual connection -- the server's endpoint is given directly.
        mesh.peerConnections().connect('tcp://127.0.0.1:7101');
        return builder.build();
      }
    })
  ]
})
class ClientModule {}

// This process's own HTTP surface -- a plain node:http server, not a NestJS
// HTTP module. See README "differences" for why.
function startHttpServer(route: ZLinkRouteClient): http.Server {
  const server = http.createServer(async (request, response) => {
    const url = new URL(request.url ?? '/', 'http://127.0.0.1');
    const match = request.method === 'GET' ? /^\/hello\/([^/]+)$/.exec(url.pathname) : null;
    if (match === null) {
      response.writeHead(404).end();
      return;
    }
    const name = decodeURIComponent(match[1]);
    try {
      const reply = await route.requestToChannel('greeting', new Hello(name)).submit<Greeting>();
      response.writeHead(200, { 'content-type': 'application/json' });
      response.end(JSON.stringify(reply.text));
    } catch (error: unknown) {
      response.writeHead(500, { 'content-type': 'application/json' });
      response.end(
        JSON.stringify({ error: error instanceof Error ? error.message : String(error) })
      );
    }
  });
  server.listen(5080, '127.0.0.1');
  return server;
}

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(ClientModule, {
    logger: ['error', 'warn', 'log']
  });
  const route = app.get<ZLinkRouteClient>(ZLINK_ROUTE_CLIENT, { strict: false });
  startHttpServer(route);
  console.log('client listening on http://127.0.0.1:5080');
}

main().catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
