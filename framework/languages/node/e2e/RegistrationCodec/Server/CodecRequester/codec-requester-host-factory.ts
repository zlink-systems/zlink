import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import type { ZLinkRouteClient } from '@zlink-systems/framework';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { zlinkProtobufCodec } from '@zlink-systems/framework-codec-protobuf/framework';
import { ZLINK_ROUTE_CLIENT, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import { RegistrationCodecNames } from '../../Shared/messages';
import { validateCodecRequesterOptions, type CodecRequesterOptions } from './Configuration/codec-requester-options';
import { REGISTRATION_CODEC_OPTIONS, createRegistrationCodecConfigurationModule } from '../../configuration';
import { createCodecRequesterEndpoints } from './Endpoints/codec-requester-endpoints';
import { createOperationalEndpoints } from './Endpoints/operational-endpoints';
import { closeHttpServer, startHttpServer } from './Support/http-server';

export async function startCodecRequester(): Promise<void> {
  let stopping = false;
  const RequesterModule = createCodecRequesterModule();
  const app = await NestFactory.createApplicationContext(RequesterModule, { logger: false, abortOnError: false });
  const options = app.get(REGISTRATION_CODEC_OPTIONS, { strict: false }) as CodecRequesterOptions;
  const channel = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const server = await startHttpServer(options.httpUrl, [
    ...createOperationalEndpoints('codec-requester', options.rid, () => { stopping = true; }),
    ...createCodecRequesterEndpoints(channel)
  ]);
  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function createCodecRequesterModule(): Function {
  class CodecRequesterModule {}
  const configuration = createRegistrationCodecConfigurationModule(validateCodecRequesterOptions);
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [REGISTRATION_CODEC_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as CodecRequesterOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          const builder = zlinkFramework();
          builder
            .codecs()
              .use(zlinkProtobufCodec())
            .configureDispatch()
              .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
              .traceLogFile(`${options.logDir}/${options.rid}-flow.log`)
              .traceLabel(options.rid);
          const mesh = builder.addRouteMesh(RegistrationCodecNames.channel);
          mesh.peerConnections().connect(options.targetEndpoint);
          mesh.channel(RegistrationCodecNames.channel).client();
          return builder.build();
        }
      })
    ]
  })(CodecRequesterModule);
  return CodecRequesterModule;
}
