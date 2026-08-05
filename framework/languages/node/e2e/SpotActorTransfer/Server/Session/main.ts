import path from 'node:path';
import { Injectable, Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkMessageFlowLogMode,
  ZLinkPeerState,
  type ActorRef,
  type ZLinkMessage,
  type ZLinkLocationRuntimeQuery,
  type ZLinkSession,
  type ZLinkSessionContext,
  type ZLinkSessionDispatchContext,
  type ZLinkSessionFactory,
  type ZLinkRouteMeshRuntime
} from '@zlink-systems/framework';
import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import {
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import {
  SpotActorTransferNames,
  type BindActorSessionReq,
  type BindActorSessionRes
} from '../../Shared/messages';
import { closeHttpServer, startHttpServer } from '../Support/http-server';
import { EvidenceStore } from '../Support/evidence-store';
import {
  SPOT_ACTOR_TRANSFER_OPTIONS,
  createSpotActorTransferConfigurationModule,
  validateServerOptions
} from '../../configuration';
import type { ServerOptions } from '../../configuration';

let options: ServerOptions;
let evidence: EvidenceStore;
let stopping = false;
process.once('SIGINT', () => { stopping = true; });
process.once('SIGTERM', () => { stopping = true; });

class GatewaySession implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext) {}

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage, signal?: AbortSignal): Promise<void> {
    if (dispatch.packetName === SpotActorTransferNames.packetBindActor) {
      const request = payload.decode<BindActorSessionReq>(Object as never);
      if (
        request.nodeRid === undefined
        || request.objectGeneration === undefined
        || request.meshName === undefined
      ) {
        throw new Error('Session gateway bind requires an exact ActorRef.');
      }
      const actor = {
        actorId: request.actorId,
        objectGeneration: BigInt(request.objectGeneration),
        meshName: request.meshName,
        nodeRid: request.nodeRid,
      } as ActorRef;
      evidence.correlate(request.actorId, request.transferId);
      await this.context.actors.bindOrGet(actor, signal);
      evidence.add(
        request.scenario,
        request.actorId,
        'session_bound',
        `gateway=${options.rid}|node=${String(actor.nodeRid)}`
          + `|generation=${actor.objectGeneration}`
      );
      this.context.client.reply({
        scenario: request.scenario,
        actorId: actor.actorId,
        nodeRid: String(actor.nodeRid),
        objectGeneration: actor.objectGeneration.toString(),
        meshName: actor.meshName
      } satisfies BindActorSessionRes).submit();
      return;
    }
    const actor = this.context.actors.bound[0];
    if (actor === undefined) throw new Error('No actor is bound.');
    await actor.relay(payload, signal);
  }
}

@Injectable()
class GatewaySessionFactory implements ZLinkSessionFactory<GatewaySession> {
  async create(context: ZLinkSessionContext): Promise<GatewaySession> { return new GatewaySession(context); }
}

class SessionModule {}

const configuration = createSpotActorTransferConfigurationModule(
  SPOT_ACTOR_TRANSFER_OPTIONS,
  validateServerOptions
);
Module({
  imports: [
    configuration,
    ZLinkModule.forRootFactory({
      imports: [configuration],
      inject: [SPOT_ACTOR_TRANSFER_OPTIONS],
      useFactory: (value: unknown) => {
        options = value as ServerOptions;
        if (options.streamEndpoint === undefined) {
          throw new Error("Configuration value 'e2e.streamEndpoint' is required for the session host.");
        }
        evidence = new EvidenceStore(options.rid, options.evidenceFile);
        const builder = zlinkFramework();
        builder.addLocationStore(new ZLinkRedisLocationStore({
          url: `redis://${options.redisEndpoint}`,
          keyPrefix: `${options.redisKeyPrefix}:location`
        }));
        builder.configureLocations()
          .pollingIntervalMs(100)
          .ownerLeaseRenewIntervalMs(1000)
          .ownerLeaseTtlMs(5000)
          .ownerLeaseFencingMarginMs(500)
          .ownerLeaseRenewTimeoutMs(500);
        builder.configureDispatch()
          .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
          .traceLogFile(path.join(options.logDir, `${options.rid}-flow.log`))
          .traceLabel(options.rid);
        const mesh = builder.addRouteMesh(SpotActorTransferNames.mesh)
          .listen(options.routerEndpoint).routingId(options.rid);
        mesh.objects().client();
        mesh.channel(SpotActorTransferNames.mesh).server();
        builder.addStreamNode(`${SpotActorTransferNames.mesh}-${options.rid}`)
          .bind(options.streamEndpoint)
          .enableActorDispatch()
          .registerSession(GatewaySessionFactory);
        return builder.build();
      }
    })
  ],
  providers: [GatewaySessionFactory]
})(SessionModule);

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(SessionModule, { logger: false, abortOnError: false });
  const routeMeshRuntime = app.get(
    ZLINK_ROUTE_MESH_RUNTIME,
    { strict: false }
  ) as ZLinkRouteMeshRuntime;
  const locationQuery = app.get(
    ZLINK_LOCATION_RUNTIME_QUERY,
    { strict: false }
  ) as ZLinkLocationRuntimeQuery;
  const server = await startHttpServer(options.httpUrl, [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ok', rid: options.rid }) },
    {
      method: 'GET',
      path: '/mesh-snapshot',
      handle: async () => {
        const snapshot = routeMeshRuntime.snapshot(SpotActorTransferNames.mesh);
        const topology = await locationQuery.listTopology(
          { meshName: SpotActorTransferNames.mesh },
          { pageSize: 100 }
        );
        return {
          rid: options.rid,
          ready: routeMeshRuntime.isReady(SpotActorTransferNames.mesh),
          readyPeerRids: snapshot.peers
            .filter(peer => peer.state === ZLinkPeerState.Ready)
            .map(peer => String(peer.nodeRid)),
          peers: snapshot.peers.map(peer => ({
            rid: String(peer.nodeRid),
            state: String(peer.state),
            ready: peer.state === ZLinkPeerState.Ready,
            lastFailure: peer.unavailableReason === undefined
              ? undefined
              : String(peer.unavailableReason)
          })),
          topologyRids: topology.items.map(entry => String(entry.nodeRid)),
          topology: topology.items.map(entry => ({
            rid: String(entry.nodeRid),
            endpoint: entry.endpoint,
            state: String(entry.state),
            draining: entry.draining
          }))
        };
      }
    },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    { method: 'POST', path: '/shutdown', handle: () => { stopping = true; return { status: 'stopping' }; } }
  ]);
  while (!stopping) await new Promise((resolve) => setTimeout(resolve, 100));
  await closeHttpServer(server);
  await app.close();
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
