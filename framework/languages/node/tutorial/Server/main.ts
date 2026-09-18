import 'reflect-metadata';
import * as http from 'node:http';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import type { ZLinkRouteMeshRuntimeOptions } from '@zlink-systems/framework';
import {
  ZLinkRedisLocationStore,
  ZLinkRedisRelocationStore
} from '@zlink-systems/framework-locations-redis';
import { PacketNames, TutorialNames } from '../Shared/contracts';
import { GetPlayerProfileHandler } from './Channel/get-player-profile-handler';
import { IssueSessionTicketHandler } from './Channel/issue-session-ticket-handler';
import { MaintenanceNoticeSubscriber } from './Channel/maintenance-notice-subscriber';
import { RecordLoginHandler } from './Channel/record-login-handler';
import { CallLogFilter } from './Dispatch/call-log-filter';
import { NodeStatusHandler } from './Ops/node-status-handler';
import { GameRoom, GetRoomStateHandler, PostChatHandler } from './Spots/game-room';
import { LobbySpot } from './Spots/lobby-spot';
import { ChangeNicknameHandler, GetPlayerHandler, Player, PlayerFactory } from './Actors/player';
import { AuthenticateHandler } from './Sessions/authenticate-handler';
import { GameSessionFactory } from './Sessions/game-session';
import { PingHandler } from './Sessions/ping-handler';

// This process opens HTTP only for the one admin route below. Every message
// call still arrives over the mesh.

@Module({
  imports: [
    ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = zlinkFramework();

        // --8<-- [start:location-store]
        // Rooms are addressed by id, not by host, so their current location is
        // kept here. Every node reads and writes the same store under the same
        // prefix.
        builder.addLocationStore(new ZLinkRedisLocationStore({
          url: 'redis://127.0.0.1:6379',
          keyPrefix: 'zlink-tutorial-node:location'
        }));
        // --8<-- [end:location-store]

        // --8<-- [start:relocation-store]
        // Registering any Spot factory requires this store, even with relocation
        // turned off: the registration itself is the condition.
        builder.addRelocationStore(new ZLinkRedisRelocationStore({
          url: 'redis://127.0.0.1:6379',
          keyPrefix: 'zlink-tutorial-node:relocation'
        }));
        // --8<-- [end:relocation-store]

        // --8<-- [start:filter-register]
        // Registration order is execution order. Filters wrap handlers this node
        // receives; Spot and Actor handlers are not covered.
        builder.options({ filters: [CallLogFilter] });
        // --8<-- [end:filter-register]

        // --8<-- [start:mesh-register]
        // Both sides must name the mesh identically, or they never see each other
        // as peers. The routing id names this node; without it the Framework assigns
        // a generated one, which a caller cannot type into a URL. A wildcard bind
        // host needs an advertise host of its own.
        const mesh = builder.addRouteMesh(TutorialNames.mesh)
          .listen('tcp://0.0.0.0:7701')
          .setAdvertiseHost('127.0.0.1')
          .routingId(TutorialNames.serverRoutingId);
        // --8<-- [end:mesh-register]

        // --8<-- [start:channel-register]
        // Only handlers exposed here can be called by other nodes. A handler class
        // sitting in the same project but left out stays unreachable. The packet
        // name is given explicitly, and it is the name the caller's payload class
        // carries.
        mesh.channel(TutorialNames.profileChannel).server()
          .addRequestHandler(PacketNames.getPlayerProfile, GetPlayerProfileHandler)
          .addSendHandler(PacketNames.recordLogin, RecordLoginHandler);
        // --8<-- [end:channel-register]

        // --8<-- [start:node-direct-register]
        // Registered on the mesh itself, with no channel(...) call. Handlers added
        // this way are reached by routing id instead of by channel name.
        mesh.addRequestHandler(PacketNames.getNodeStatus, NodeStatusHandler);
        // --8<-- [end:node-direct-register]

        // --8<-- [start:clientserver-register]
        // The caller dials this endpoint directly, so it needs a port of its own and
        // an address to advertise, separate from the mesh.
        builder.addClientServerChannel(TutorialNames.ticketingChannel)
          .server()
          .listen(7711)
          .setBindHost('127.0.0.1')
          .setAdvertiseHost('127.0.0.1')
          .addRequestHandler(PacketNames.issueSessionTicket, IssueSessionTicketHandler);
        // --8<-- [end:clientserver-register]

        // --8<-- [start:fanout-subscribe]
        // The publisher endpoint is given here because this process runs without a
        // Location Store. With one registered, enableSubscriber() takes no argument
        // and finds every publisher of this channel instead.
        builder.addFanoutChannel(TutorialNames.broadcastChannel)
          .enableSubscriber('tcp://127.0.0.1:7712')
          .addPublishHandler(PacketNames.maintenanceNotice, MaintenanceNoticeSubscriber);
        // --8<-- [end:fanout-subscribe]

        // --8<-- [start:object-server]
        // A mesh node picks this role once. Keep the builder and reuse it,
        // because calling objects().server() a second time is rejected at
        // startup.
        const objects = mesh.objects().server();
        // --8<-- [end:object-server]

        // --8<-- [start:spot-register]
        // The stable type is what a caller names when opening a room. Any node
        // that registers it is a candidate to host one. Exactly one relocation
        // policy is required; moving a live room to another node is a separate
        // topic.
        objects.addSpotFactory(
          TutorialNames.gameRoomType,
          GameRoom,
          (factory) => factory.disableRelocation()
        );
        // --8<-- [end:spot-register]

        // --8<-- [start:actor-register]
        // One lobby per object server. Newly created players start there.
        objects.addEntrySpot(LobbySpot);

        // Nodes that register the player type are candidates to host one.
        objects.addActorFactory(
          TutorialNames.playerActorType,
          PlayerFactory,
          (factory) => factory.disableRelocation()
        );
        // --8<-- [end:actor-register]

        // --8<-- [start:stream-register]
        // The port game clients connect to. One session type per stream node,
        // and actor dispatch must be on for a session to relay to its player.
        // The Node stream transport is a WebSocket, so the endpoint is ws://.
        builder.addStreamNode(TutorialNames.clientStreamNode)
          .enableActorDispatch()
          .bind('ws://0.0.0.0:7721')
          .registerSession(GameSessionFactory);
        // --8<-- [end:stream-register]

        return builder.build();
      }
    })
  ],
  // Nest constructs every handler and filter, so each one is listed here.
  providers: [
    CallLogFilter,
    GetPlayerProfileHandler,
    RecordLoginHandler,
    IssueSessionTicketHandler,
    MaintenanceNoticeSubscriber,
    NodeStatusHandler,
    GameRoom,
    PostChatHandler,
    GetRoomStateHandler,
    LobbySpot,
    Player,
    PlayerFactory,
    ChangeNicknameHandler,
    GetPlayerHandler,
    GameSessionFactory,
    PingHandler,
    AuthenticateHandler
  ]
})
class ServerModule {}

// --8<-- [start:weight-runtime]
// Weight is the one value this node can change while running. 0 keeps the
// socket open and finishes in-flight work, but other nodes stop choosing this
// one for new calls. 100 is the normal value.
const weightRoute = /^\/admin\/channels\/([^/]+)\/weight$/;

function setChannelWeight(
  mesh: ZLinkRouteMeshRuntimeOptions,
  channel: string,
  value: number
): { channel: string; weight: number } {
  mesh.channel(channel).weight = value;
  return { channel, weight: value };
}
// --8<-- [end:weight-runtime]

// This process's own HTTP surface -- a plain node:http server, the same shape
// the client uses, carrying the one route above. The new weight arrives in the
// query string because the route has no body: POST
// /admin/channels/{channel}/weight?value=N
function startAdminHttpServer(mesh: ZLinkRouteMeshRuntimeOptions): http.Server {
  const server = http.createServer((request, response) => {
    const url = new URL(request.url ?? '/', 'http://127.0.0.1');
    const matched = weightRoute.exec(url.pathname);
    if (request.method !== 'POST' || matched === null) {
      response.writeHead(404).end();
      return;
    }
    try {
      const result = setChannelWeight(
        mesh,
        decodeURIComponent(matched[1]),
        Number(url.searchParams.get('value'))
      );
      response.writeHead(200, { 'content-type': 'application/json' });
      response.end(JSON.stringify(result));
    } catch (error: unknown) {
      response.writeHead(400, { 'content-type': 'application/json' });
      response.end(JSON.stringify({
        error: error instanceof Error ? error.message : String(error)
      }));
    }
  });
  server.listen(5481, '127.0.0.1');
  return server;
}

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(ServerModule, { logger: ['error', 'warn', 'log'] });
  startAdminHttpServer(
    app.get<ZLinkRouteMeshRuntimeOptions>(ZLINK_ROUTE_MESH_RUNTIME_OPTIONS, { strict: false })
  );
  console.log(
    `server listening on tcp://0.0.0.0:7701 (mesh "${TutorialNames.mesh}",`
    + ` routing id "${TutorialNames.serverRoutingId}")`
  );
  console.log('server admin listening on http://127.0.0.1:5481');
}

main().catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
