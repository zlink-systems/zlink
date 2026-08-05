import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import type { ZLinkRouteClient } from '@zlink-systems/framework';
import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import { ZLINK_ROUTE_CLIENT, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import { SpotServiceNames } from '../../Shared/messages';
import { createSpotServiceConfigurationModule } from '../../configuration';
import { validateSessionOptions } from './Configuration/session-options';
import type { SessionOptions } from './Configuration/session-options';
import { createSessionEndpoints } from './Endpoints/session-endpoints';
import { ScenarioSessionFactory } from './Handlers/scenario-session';
import { EvidenceStore } from './Infrastructure/evidence-store';
import { closeHttpServer, startHttpServer } from './Support/http-server';

const SESSION_OPTIONS = Symbol.for('SPOT_SERVICE_SESSION_OPTIONS');

export async function startSessionHost(): Promise<void> {
  const configuration = createSpotServiceConfigurationModule(SESSION_OPTIONS, validateSessionOptions);
  const createEvidence = (options: SessionOptions): EvidenceStore => {
    fs.mkdirSync(options.logDir, { recursive: true });
    return new EvidenceStore(options.rid, options.evidenceFile);
  };
  let stopping = false;

  class SessionModule {}
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [SESSION_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as SessionOptions;
          const builder = zlinkFramework();
          builder.addLocationStore(new ZLinkRedisLocationStore({
            url: `redis://${options.redisEndpoint}`,
            keyPrefix: options.redisKeyPrefix
          }));
          builder.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
            .traceLogFile(`${options.logDir}/${options.rid}-flow.log`)
            .traceLabel(options.rid);

          const controlMesh = builder.addRouteMesh(SpotServiceNames.controlChannel)
            .listen(options.controlRouterEndpoint)
            .routingId(options.rid);
          controlMesh.channel(SpotServiceNames.controlChannel).client();
          for (const [peerRid, endpoint] of options.playControlEndpoints) {
            controlMesh.peerConnections().connect(peerRid, endpoint);
          }
          const spotMesh = builder.addRouteMesh(SpotServiceNames.spotChannel)
            .listen(options.spotRouterEndpoint)
            .routingId(options.rid);
          spotMesh.objects().client();
          spotMesh.channel(SpotServiceNames.spotChannel).client();
          for (const [peerRid, endpoint] of options.playSpotRouterEndpoints) {
            spotMesh.peerConnections().connect(peerRid, endpoint);
          }
          const alternateObjectMesh = builder
            .addRouteMesh(SpotServiceNames.spotOnlyMesh)
            .listen(options.alternateObjectRouterEndpoint)
            .routingId(options.rid);
          alternateObjectMesh.objects().client();
          alternateObjectMesh.channel(SpotServiceNames.spotOnlyMesh).client();
          for (const [peerRid, endpoint] of options.playAlternateObjectRouterEndpoints) {
            alternateObjectMesh.peerConnections().connect(peerRid, endpoint);
          }
          builder.addStreamNode(SpotServiceNames.streamNode)
            .bind(options.streamEndpoint)
            .enableActorDispatch()
            .registerSession(ScenarioSessionFactory);
          if (options.tlsStreamEndpoint !== undefined) {
            if (options.tlsCertPath === undefined || options.tlsKeyPath === undefined) {
              throw new Error('--tls-cert-path and --tls-key-path are required with --tls-stream-endpoint.');
            }
            builder.addStreamNode(SpotServiceNames.tlsStreamNode)
              .bind(options.tlsStreamEndpoint)
              .enableActorDispatch()
              .setTlsServer(options.tlsCertPath, options.tlsKeyPath)
              .registerSession(ScenarioSessionFactory);
          }

          return builder.build();
        }
      })
    ],
    providers: [
      { provide: EvidenceStore, inject: [SESSION_OPTIONS], useFactory: createEvidence },
      ScenarioSessionFactory
    ]
  })(SessionModule);

  const app = await NestFactory.createApplicationContext(SessionModule, { logger: false, abortOnError: false });
  const options = app.get(SESSION_OPTIONS) as SessionOptions;
  const evidence = app.get(EvidenceStore);
  const route = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const server = await startHttpServer(options.httpUrl, createSessionEndpoints(evidence, route, () => { stopping = true; }));
  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}
