import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLINK_ROUTE_CLIENT, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import type { ZLinkRouteClient } from '@zlink-systems/framework';
import { createRedisLocationStore, locationMessagingOptions } from '../../Shared/location-store';
import { PacketNames } from '../../Shared/messages';
import { validateServerOptions } from './Configuration/server-options';
import type { ServerOptions } from './Configuration/server-options';
import { REGISTRY_MESSAGING_OPTIONS, createRegistryMessagingConfigurationModule } from '../../configuration';
import { createWorkflowEndpoints } from './Endpoints/workflow-endpoints';
import { EvidenceDispatchErrorObserver, WorkflowRequestHandler } from './Handlers/workflow-handlers';
import { EvidenceStore } from './Infrastructure/evidence-store';
import { closeHttpServer, startHttpServer } from './Support/http-server';

export async function startWorkflowHost(): Promise<void> {
  let stopping = false;
  const WorkflowModule = createWorkflowModule();
  const app = await NestFactory.createApplicationContext(WorkflowModule, { logger: false, abortOnError: false });
  const options = app.get(REGISTRY_MESSAGING_OPTIONS, { strict: false }) as ServerOptions;
  const evidence = app.get(EvidenceStore, { strict: false });
  const channel = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const server = await startHttpServer(options.httpUrl, createWorkflowEndpoints(evidence, channel, () => { stopping = true; }));

  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function createWorkflowModule(): Function {
  class WorkflowModule {}
  const configuration = createRegistryMessagingConfigurationModule(validateServerOptions);
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration], inject: [REGISTRY_MESSAGING_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as ServerOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          const builder = zlinkFramework();
          builder
            .configureDispatch()
              .setMessageFlowObserver(EvidenceDispatchErrorObserver)
              .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
              .traceLogFile(`${options.logDir}/${options.rid}-flow.log`)
              .traceLabel(options.rid);
          if (options.redisEndpoint !== undefined && options.redisKeyPrefix !== undefined) {
            builder.addLocationStore(createRedisLocationStore({
              redisEndpoint: options.redisEndpoint,
              redisKeyPrefix: options.redisKeyPrefix
            }));
            locationMessagingOptions(builder.configureLocations());
          }
          const workflow = builder.addRouteMesh('workflow')
            .listen(options.workflowEndpoint)
            .routingId(options.rid);
          workflow.peerConnections();
          workflow.channel('workflow').server()
            .addRequestHandler(PacketNames.workflowReq, WorkflowRequestHandler);
          return builder.build();
        }
      })
    ],
    providers: [
      { provide: EvidenceStore, inject: [REGISTRY_MESSAGING_OPTIONS], useFactory: (value: unknown) => {
        const options = value as ServerOptions; return new EvidenceStore(options.rid, options.evidenceFile);
      } },
      EvidenceDispatchErrorObserver,
      WorkflowRequestHandler
    ]
  })(WorkflowModule);
  return WorkflowModule;
}
