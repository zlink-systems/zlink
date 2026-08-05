import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import { AutomaticTurnDispatchNames } from '../../Shared/messages';
import { EvidenceStore } from './Support/evidence-store';
import { closeHttpServer, startHttpServer } from './Support/http-server';
import { createAutomaticTurnConfigurationModule } from '../../configuration';
import { DELAY_OPTIONS, validateDelayOptions } from './Configuration/delay-options';
import type { DelayOptions } from './Configuration/delay-options';
import { DelayHandler } from './Handlers/delay-handler';

export async function startDelayHost(): Promise<void> {
  let stopping = false;
  const configuration = createAutomaticTurnConfigurationModule(DELAY_OPTIONS, validateDelayOptions);

  class DelayModule {}
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [DELAY_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as DelayOptions;
          const builder = zlinkFramework();
          const delay = builder.addRouteMesh(AutomaticTurnDispatchNames.delayChannel)
            .listen(options.delayEndpoint)
            .routingId(options.rid);
          delay.channel(AutomaticTurnDispatchNames.delayChannel).server()
            .addRequestHandler('DelayReq', DelayHandler);
          return builder.build();
        }
      })
    ],
    providers: [
      {
        provide: EvidenceStore,
        inject: [DELAY_OPTIONS],
        useFactory: (options: DelayOptions) => new EvidenceStore(options.rid, options.evidenceFile)
      },
      DelayHandler
    ]
  })(DelayModule);

  const app = await NestFactory.createApplicationContext(DelayModule, { logger: false });
  const options = app.get<DelayOptions>(DELAY_OPTIONS);
  const evidence = app.get(EvidenceStore);
  const server = await startHttpServer(options.httpUrl, [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'delay', rid: options.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    {
      method: 'POST',
      path: '/shutdown',
      handle: () => {
        stopping = true;
        return { status: 'stopping' };
      }
    }
  ]);
  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}
