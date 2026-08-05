import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import type { ZLinkRouteClient } from '@zlink-systems/framework';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLINK_ROUTE_CLIENT, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import { EchoJsonReq, PacketNames, RegistrationCodecNames } from '../../Shared/messages';
import { validateJsonOnlyOptions, type JsonOnlyOptions } from './Configuration/json-only-options';
import { REGISTRATION_CODEC_OPTIONS, createRegistrationCodecConfigurationModule } from '../../configuration';
import { createOperationalEndpoints } from './Endpoints/operational-endpoints';
import { JsonOnlyEchoRequestHandler } from './Handlers/json-only-handlers';
import { EvidenceStore } from './Infrastructure/evidence-store';
import { closeHttpServer, startHttpServer } from './Support/http-server';

export async function startJsonOnlyPeer(): Promise<void> {
  let stopping = false;
  const PeerModule = createJsonOnlyModule();
  const app = await NestFactory.createApplicationContext(PeerModule, { logger: false, abortOnError: false });
  const options = app.get(REGISTRATION_CODEC_OPTIONS, { strict: false }) as JsonOnlyOptions;
  const evidence = app.get(EvidenceStore, { strict: false });
  const channel = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const server = await startHttpServer(options.httpUrl, [
    ...createOperationalEndpoints(evidence, () => { stopping = true; }),
    {
      method: 'POST',
      path: '/codec/json-recovery',
      handle: async () => {
        const reply = await channel.requestToChannel(RegistrationCodecNames.channel, new EchoJsonReq('rc-b5-json'))
          .submit();
        evidence.add('codec-mismatch-json-recovery|status=ok');
        return reply;
      }
    }
  ]);
  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function createJsonOnlyModule(): Function {
  class JsonOnlyModule {}
  const configuration = createRegistrationCodecConfigurationModule(validateJsonOnlyOptions);
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [REGISTRATION_CODEC_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as JsonOnlyOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          const builder = zlinkFramework();
          builder
            .configureDispatch()
              .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
              .traceLogFile(`${options.logDir}/${options.rid}-flow.log`)
              .traceLabel(options.rid);
          const mesh = builder.addRouteMesh(RegistrationCodecNames.channel)
            .listen(options.channelEndpoint);
          mesh.peerConnections().connect(options.channelEndpoint);
          mesh.channel(RegistrationCodecNames.channel).server()
            .addRequestHandler(PacketNames.echoJsonReq, JsonOnlyEchoRequestHandler);
          return builder.build();
        }
      })
    ],
    providers: [
      {
        provide: EvidenceStore,
        inject: [REGISTRATION_CODEC_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as JsonOnlyOptions;
          return new EvidenceStore(options.rid, options.evidenceFile);
        }
      },
      JsonOnlyEchoRequestHandler
    ]
  })(JsonOnlyModule);
  return JsonOnlyModule;
}
