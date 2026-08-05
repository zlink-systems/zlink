import * as fs from 'node:fs';
import { Inject, Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  type ZLinkClientServerRuntime,
  type ZLinkChannelClient,
  type ZLinkFrameworkRuntime,
  type ZLinkRouteClient,
  type ZLinkRouteMeshRuntime,
  type ZLinkSpotOutbound,
  type ZLinkSpotRequestCall
} from '@zlink-systems/framework';
import {
  ZLINK_CLIENT_SERVER_RUNTIME,
  ZLINK_CHANNEL_CLIENT,
  ZLINK_FRAMEWORK_RUNTIME,
  ZLINK_ROUTE_CLIENT,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_SPOT_OUTBOUND,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import { createRedisLocationStore, createRedisRelocationStore, locationOptions } from '../Shared/location-store';
import { ChannelEgressNames, ChannelProbeMsg, ChannelProbeReq, SpotWorkflowReq, type EvidenceWaitReq, type InvokeReq, type SendRes, type InvokeRes } from '../Shared/messages';
import { createChannelEgressConfiguration, CHANNEL_EGRESS_OPTIONS } from './Configuration/module';
import { validateRoleOptions, type RoleOptions } from './Configuration/options';
import { ChannelProbeRequestHandler, ChannelProbeSendHandler, publicErrorKind } from './Handlers/route-handlers';
import { DispatchErrorObserver } from './Handlers/dispatch-error-observer';
import { SpotWorkflowHandler, SpotWorkflowTimerHandler } from './Handlers/spot-handlers';
import { Config12Spot } from './Spots/config12-spot';
import { EvidenceStore } from './Support/evidence-store';
import { RoleState } from './Support/role-state';
import { closeHttpServer, startHttpServer, type HttpRoute } from './Support/http-server';

export async function startRoleHost(): Promise<void> {
  let stopping = false;
  const RoleModule = createRoleModule();
  const app = await NestFactory.createApplicationContext(RoleModule, { logger: false, abortOnError: false });
  const options = app.get(CHANNEL_EGRESS_OPTIONS) as RoleOptions;
  const evidence = app.get(EvidenceStore);
  const state = app.get(RoleState);
  const routes = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const channels = app.get(ZLINK_CHANNEL_CLIENT, { strict: false }) as ZLinkChannelClient;
  const frameworkRuntime = app.get(ZLINK_FRAMEWORK_RUNTIME) as ZLinkFrameworkRuntime;
  const spotOutbound = app.get(ZLINK_SPOT_OUTBOUND, { strict: false }) as ZLinkSpotOutbound | undefined;
  const routeRuntime = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;
  const clientServerRuntime = app.get(ZLINK_CLIENT_SERVER_RUNTIME, { strict: false }) as ZLinkClientServerRuntime;
  const server = await startHttpServer(options.httpUrl, createRoutes(
    options,
    evidence,
    state,
    channels,
    routes,
    spotOutbound,
    routeRuntime,
    clientServerRuntime,
    frameworkRuntime,
    () => { stopping = true; }
  ));

  while (!stopping) await new Promise((resolve) => setTimeout(resolve, 50));
  await closeHttpServer(server);
  await app.close();
}

function createRoleModule(): Function {
  class RoleModule {}
  const configuration = createChannelEgressConfiguration(validateRoleOptions);
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [CHANNEL_EGRESS_OPTIONS],
        useFactory: (value: unknown) => buildFramework(value as RoleOptions)
      })
    ],
    providers: [
      {
        provide: EvidenceStore,
        inject: [CHANNEL_EGRESS_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as RoleOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          return new EvidenceStore(options.rid, options.role, options.instanceMarker, options.evidenceFile);
        }
      },
      { provide: RoleState, useClass: RoleState },
      DispatchErrorObserver,
      ChannelProbeRequestHandler,
      ChannelProbeSendHandler,
      SpotWorkflowHandler,
      SpotWorkflowTimerHandler,
      Config12Spot
    ]
  })(RoleModule);
  return RoleModule;
}

function buildFramework(options: RoleOptions) {
  const builder = zlinkFramework();
  builder.configureDispatch()
    .setMessageFlowObserver(DispatchErrorObserver)
    .traceLabel(options.rid)
    .traceLogFile(`${options.logDir}/${options.rid}-flow.log`);
  builder.addLocationStore(createRedisLocationStore({
    redisEndpoint: options.redisEndpoint,
    redisKeyPrefix: options.redisKeyPrefix
  }));
  builder.addRelocationStore(createRedisRelocationStore({
    redisEndpoint: options.redisEndpoint,
    redisKeyPrefix: options.redisKeyPrefix
  }));
  locationOptions(builder.configureLocations());

  const game = builder.addRouteMesh(ChannelEgressNames.gameMesh)
    .listen(options.gameEndpoint)
    .routingId(options.rid);
  game.peerConnections();
  registerRouteChannels(game, options.gameServers, options.gameClients);

  const audit = builder.addRouteMesh(ChannelEgressNames.auditMesh)
    .listen(options.auditEndpoint)
    .routingId(`${options.rid}-audit`);
  audit.peerConnections();
  registerRouteChannels(audit, options.auditServers, options.auditClients);

  if (options.role === 'play') {
    game.objects().server().addInstanceSpotFactory(
      ChannelEgressNames.spotType,
      Config12Spot,
      (factory) => factory.disableRelocation()
    );
  } else if (options.role === 'caller') {
    game.objects().client();
  }

  if (options.workflowClient || options.workflowServer) {
    const workflow = builder.addClientServerChannel(ChannelEgressNames.workflow);
    if (options.workflowClient) workflow.client();
    if (options.workflowServer) {
      workflow.server()
        .listen(options.workflowPort)
        .setBindHost('127.0.0.1')
        .setAdvertiseHost('127.0.0.1')
        .setWeight(options.workflowWeight)
        .addRequestHandler('ChannelProbeReq', ChannelProbeRequestHandler)
        .addRequestHandler('SpotWorkflowReq', ChannelProbeRequestHandler)
        .addSendHandler('ChannelProbeMsg', ChannelProbeSendHandler);
    }
    if (options.invalidMode === 'duplicate-workflow-client') workflow.client();
  }

  if (options.invalidMode === 'route-clientserver-conflict') {
    builder.addClientServerChannel(ChannelEgressNames.play).client();
  }
  return builder.build();
}

function registerRouteChannels(
  mesh: ReturnType<ReturnType<typeof zlinkFramework>['addRouteMesh']>,
  servers: readonly string[],
  clients: readonly string[]
): void {
  for (const name of servers) {
    mesh.channel(name).server()
      .addRequestHandler('ChannelProbeReq', ChannelProbeRequestHandler)
      .addSendHandler('ChannelProbeMsg', ChannelProbeSendHandler);
  }
  for (const name of clients) mesh.channel(name).client();
}

function createRoutes(
  options: RoleOptions,
  evidence: EvidenceStore,
  state: RoleState,
  channels: ZLinkChannelClient,
  routes: ZLinkRouteClient,
  spotOutbound: ZLinkSpotOutbound | undefined,
  routeRuntime: ZLinkRouteMeshRuntime,
  clientServerRuntime: ZLinkClientServerRuntime,
  frameworkRuntime: ZLinkFrameworkRuntime,
  stop: () => void
): HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: options.role, rid: options.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    {
      method: 'POST', path: '/evidence/wait', handle: (body) => {
        const request = body as EvidenceWaitReq;
        return evidence.waitUntil(
          (entries) => entries.some((entry) => entry.includes(request.contains)),
          Math.min(Math.max(request.timeoutMilliseconds ?? 5000, 1), 30_000)
        );
      }
    },
    {
      method: 'POST', path: '/request', handle: async (body): Promise<InvokeRes> => {
        const request = body as InvokeReq;
        const started = Date.now();
        try {
          const client = request.channel === ChannelEgressNames.workflow ? channels : routes;
          const reply = await client.requestToChannel(
            request.channel,
            new ChannelProbeReq(request.id, request.mode ?? 'echo')
          ).timeout(5000).submit();
          return { succeeded: true, reply: reply as never, elapsedMilliseconds: Date.now() - started };
        } catch (error) {
          return { succeeded: false, error: publicErrorKind(error), elapsedMilliseconds: Date.now() - started };
        }
      }
    },
    {
      method: 'POST', path: '/send', handle: async (body): Promise<SendRes> => {
        const request = body as InvokeReq;
        const started = Date.now();
        try {
          const client = request.channel === ChannelEgressNames.workflow ? channels : routes;
          await client.sendToChannel(request.channel, new ChannelProbeMsg(request.id)).submit();
          return { succeeded: true, elapsedMilliseconds: Date.now() - started };
        } catch (error) {
          return { succeeded: false, error: publicErrorKind(error), elapsedMilliseconds: Date.now() - started };
        }
      }
    },
    {
      method: 'POST', path: '/spot/workflow', handle: async (body) => {
        if (spotOutbound === undefined) throw new Error('Spot outbound is not registered.');
        const request = body as { readonly spotId: string; readonly id: string };
        evidence.add(`spot-request-start|spot=${request.spotId}|id=${request.id}`);
        const call: ZLinkSpotRequestCall = spotOutbound
          .requestToSpot(request.spotId, new SpotWorkflowReq(request.id))
          .instanceSpot(ChannelEgressNames.spotType)
          .inMesh(ChannelEgressNames.gameMesh)
          .timeout(5000);
        try {
          await call.submit<unknown>();
          return { succeeded: true };
        } catch (error) {
          evidence.add(`spot-request-error|spot=${request.spotId}|id=${request.id}|error=${publicErrorKind(error)}|message=${error instanceof Error ? error.message : String(error)}`);
          return { succeeded: false, error: publicErrorKind(error) };
        }
      }
    },
    {
      method: 'GET', path: '/status/route', handle: () => {
        const snapshot = routeRuntime.snapshot(ChannelEgressNames.gameMesh);
        return {
          state: String(snapshot.state),
          isReady: snapshot.isReady,
          readyPeerCount: snapshot.peers.filter((peer) => peer.state === 1).length,
          peers: snapshot.peers.map((peer) => ({ rid: String(peer.nodeRid), state: String(peer.state) })),
          channels: snapshot.channels,
          placement: snapshot.placement
        };
      }
    },
    {
      method: 'GET', path: '/status/workflow', handle: () => {
        const snapshot = clientServerRuntime.snapshot(ChannelEgressNames.workflow);
        return {
          state: String(snapshot.state),
          isReady: snapshot.isReady,
          readyTargetCount: snapshot.readyTargetCount,
          localRole: snapshot.localRole,
          targets: snapshot.targets.map((target) => ({
            rid: String(target.nodeRid), weight: target.weight, state: String(target.state)
          }))
        };
      }
    },
    { method: 'POST', path: '/control/hold', handle: () => { state.hold(); return { status: 'held' }; } },
    { method: 'POST', path: '/control/release', handle: () => { state.release(); return { status: 'released' }; } },
    {
      method: 'POST', path: '/drain', handle: () => {
        void frameworkRuntime.shutdown({ deadlineMs: 10_000 }).finally(stop);
        return { status: 'draining' };
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}
