import 'reflect-metadata';
import { Injectable, Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext, ZLinkRequestHandler } from '@zlink-systems/framework';
import { Greeting, Hello, PacketNames } from '../Shared/contracts';

// Handles one request on the "greeting" channel.
@Injectable()
class HelloHandler implements ZLinkRequestHandler<Hello, Greeting> {
  async handle(request: Hello, context: ZLinkMessageContext): Promise<Greeting> {
    void context;
    return { text: `hello, ${request.name}` };
  }
}

@Module({
  imports: [
    ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = zlinkFramework();
        // Names the mesh and opens this process's endpoint for peers to connect to.
        // A wildcard bind host needs an explicit advertise host -- see README
        // "differences".
        const mesh = builder.addRouteMesh('services')
          .listen('tcp://0.0.0.0:7101')
          .setAdvertiseHost('127.0.0.1');
        // This process handles the "greeting" channel. IZLinkRequestHandler classes
        // register on the channel builder explicitly -- see README "differences".
        mesh.channel('greeting').server()
          .addRequestHandler(PacketNames.hello, HelloHandler);
        return builder.build();
      }
    })
  ],
  providers: [HelloHandler]
})
class ServerModule {}

async function main(): Promise<void> {
  await NestFactory.createApplicationContext(ServerModule, { logger: ['error', 'warn', 'log'] });
  console.log('server listening on tcp://0.0.0.0:7101 (channel "greeting")');
}

main().catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
