import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import type { ZLinkRouteClient } from '@zlink-systems/framework';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import {
  createMessagePackSerializer,
  ZLINK_MESSAGEPACK_CONTENT_TYPE
} from '@zlink-systems/framework-codec-msgpack/framework';
import {
  createProtobufMessageSerializer,
  ZLINK_PROTOBUF_CONTENT_TYPE
} from '@zlink-systems/framework-codec-protobuf/framework';
import { ZLINK_ROUTE_CLIENT, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import {
  MessagePackEchoMsg,
  MessagePackEchoReq,
  PacketNames,
  ProtobufEchoMsg,
  ProtobufEchoReq,
  RegistrationCodecNames
} from '../../Shared/messages';
import { validateServerOptions, type ServerOptions } from './Configuration/server-options';
import { REGISTRATION_CODEC_OPTIONS, createRegistrationCodecConfigurationModule } from '../../configuration';
import { createMainEndpoints } from './Endpoints/main-endpoints';
import { createOperationalEndpoints } from './Endpoints/operational-endpoints';
import {
  JsonEchoCommandHandler,
  JsonEchoRequestHandler,
  MessagePackEchoCommandHandler,
  MessagePackEchoRequestHandler,
  ProtobufEchoCommandHandler,
  ProtobufEchoRequestHandler
} from './Handlers/codec-handlers';
import { DiEchoRequestHandler } from './Handlers/di-echo-handler';
import { FirstFilter, SecondFilter } from './Handlers/dispatch-filters';
import {
  DuplicateEchoRequestHandler,
  EchoAttrCommandHandler,
  EchoAttrRequestHandler,
  EchoAutoCommandHandler,
  EchoAutoRequestHandler,
  EchoManualCommandHandler,
  EchoManualRequestHandler
} from './Handlers/registration-handlers';
import { EvidenceStore } from './Infrastructure/evidence-store';
import { ScopedProbe, SingletonProbe } from './Infrastructure/lifecycle-probes';
import { closeHttpServer, startHttpServer } from './Support/http-server';

export interface MainHostOptions {
  readonly duplicate?: boolean;
}

export async function startMainHost(hostOptions: MainHostOptions = {}): Promise<void> {
  let stopping = false;

  const MainModule = createMainModule(hostOptions);
  const app = await NestFactory.createApplicationContext(MainModule, { logger: false, abortOnError: false });
  const options = app.get(REGISTRATION_CODEC_OPTIONS, { strict: false }) as ServerOptions;
  const evidence = app.get(EvidenceStore, { strict: false });
  const channel = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const server = await startHttpServer(options.httpUrl, [
    ...createOperationalEndpoints(evidence, () => { stopping = true; }),
    ...createMainEndpoints(evidence, channel)
  ]);

  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function createMainModule(hostOptions: MainHostOptions): Function {
  class MainModule {}
  const configuration = createRegistrationCodecConfigurationModule(validateServerOptions);

  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [REGISTRATION_CODEC_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as ServerOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          const builder = zlinkFramework();
          builder
            .codecs()
              .use({
                register: (codecs) => {
                  // The extension is registered once at the root. These predicates keep the
                  // sample's two non-JSON DTO families disjoint without exposing a selector API.
                  const protobufSerializer = {
                    ...createProtobufMessageSerializer(),
                    canSerialize: (value: unknown) => value instanceof ProtobufEchoReq || value instanceof ProtobufEchoMsg
                  };
                  const messagePackSerializer = {
                    ...createMessagePackSerializer(),
                    canSerialize: (value: unknown) => value instanceof MessagePackEchoReq || value instanceof MessagePackEchoMsg
                  };
                  codecs
                    .addSerializer(ZLINK_PROTOBUF_CONTENT_TYPE, protobufSerializer)
                    .addSerializer(ZLINK_MESSAGEPACK_CONTENT_TYPE, messagePackSerializer);
                }
              })
            .configureDispatch()
              .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
              .traceLogFile(`${options.logDir}/${options.rid}-flow.log`)
              .traceLabel(options.rid);
          builder.options({ filters: [FirstFilter, SecondFilter] });
          const mesh = builder.addRouteMesh(RegistrationCodecNames.channel)
            .listen(options.channelEndpoint);
          mesh.peerConnections().connect(options.channelEndpoint);
          const channel = mesh.channel(RegistrationCodecNames.channel).server()
            .addHandlerGroup('auto')
            .addHandlerGroup('attr')
            .addRequestHandler(PacketNames.echoManualReq, EchoManualRequestHandler)
            .addSendHandler(PacketNames.echoManualMsg, EchoManualCommandHandler)
            .addRequestHandler(PacketNames.echoDiReq, DiEchoRequestHandler)
            .addRequestHandler(PacketNames.echoJsonReq, JsonEchoRequestHandler)
            .addSendHandler(PacketNames.echoJsonMsg, JsonEchoCommandHandler)
            .addRequestHandler(PacketNames.echoProtobufReq, ProtobufEchoRequestHandler)
            .addSendHandler(PacketNames.echoProtobufMsg, ProtobufEchoCommandHandler)
            .addRequestHandler(PacketNames.echoMessagePackReq, MessagePackEchoRequestHandler)
            .addSendHandler(PacketNames.echoMessagePackMsg, MessagePackEchoCommandHandler);
          if (hostOptions.duplicate === true) {
            channel.addRequestHandler(PacketNames.echoManualReq, DuplicateEchoRequestHandler);
          }
          return builder.build();
        }
      })
    ],
    providers: [
      {
        provide: EvidenceStore,
        inject: [REGISTRATION_CODEC_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as ServerOptions;
          return new EvidenceStore(options.rid, options.evidenceFile);
        }
      },
      EchoAutoRequestHandler,
      EchoAutoCommandHandler,
      EchoAttrRequestHandler,
      EchoAttrCommandHandler,
      EchoManualRequestHandler,
      EchoManualCommandHandler,
      DuplicateEchoRequestHandler,
      DiEchoRequestHandler,
      JsonEchoRequestHandler,
      JsonEchoCommandHandler,
      ProtobufEchoRequestHandler,
      ProtobufEchoCommandHandler,
      MessagePackEchoRequestHandler,
      MessagePackEchoCommandHandler,
      SingletonProbe,
      ScopedProbe,
      FirstFilter,
      SecondFilter
    ]
  })(MainModule);
  return MainModule;
}
