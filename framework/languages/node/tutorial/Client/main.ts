import 'reflect-metadata';
import * as http from 'node:http';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLINK_CHANNEL_CLIENT,
  ZLINK_FANOUT_CLIENT,
  ZLINK_ROUTE_CLIENT,
  ZLINK_SPOT_MANAGER,
  ZLINK_SPOT_OUTBOUND,
  ZLINK_ACTOR_CLIENT,
  ZLINK_ACTOR_MANAGER,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import type {
  ZLinkChannelClient,
  ZLinkFanoutClient,
  ZLinkRouteClient,
  ZLinkSpotManager,
  ZLinkSpotOutbound,
  ZLinkActorClient,
  ZLinkActorManager
} from '@zlink-systems/framework';
import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import {
  ChangeNickname,
  CreatePlayer,
  GetNodeStatus,
  GetPlayer,
  GetPlayerProfile,
  GetRoomState,
  IssueSessionTicket,
  MaintenanceNotice,
  OpenRoom,
  PostChat,
  RecordLogin,
  TutorialNames,
  type NodeStatus,
  type PlayerInfo,
  type PlayerProfile,
  type RoomState,
  type SessionTicket
} from '../Shared/contracts';
import { withZLinkErrorResponse, type HttpResult } from './zlink-error-response';

// Every channel peer this process needs is named directly below. Rooms are the
// exception: they are looked up by id through the Location Store.

@Module({
  imports: [
    ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = zlinkFramework();

        // --8<-- [start:location-store-client]
        // Rooms are looked up by whoever calls them, so a node that hosts none
        // still needs the store, pointed at the same prefix.
        builder.addLocationStore(new ZLinkRedisLocationStore({
          url: 'redis://127.0.0.1:6379',
          keyPrefix: 'zlink-tutorial-node:location'
        }));
        // --8<-- [end:location-store-client]

        // --8<-- [start:channel-client-register]
        // This node opens an endpoint too. Both sides listen to become peers.
        const mesh = builder.addRouteMesh(TutorialNames.mesh)
          .listen('tcp://0.0.0.0:7702')
          .setAdvertiseHost('127.0.0.1');

        // client() means this node exposes no handler for the channel; it only calls.
        mesh.channel(TutorialNames.profileChannel).client();

        // A mesh peer connection, not a channel one. The mesh picks a node that
        // serves the channel from among the peers it learns this way, so a channel
        // call never names a node. The routing id names which node is expected at
        // that endpoint. The node-direct call below reaches that node either way
        // here -- see README, "what differs from the .NET tutorial".
        mesh.peerConnections().connect(TutorialNames.serverRoutingId, 'tcp://127.0.0.1:7701');
        // --8<-- [end:channel-client-register]

        // --8<-- [start:clientserver-client-register]
        // Here the caller decides who answers: the server it dialed. Mesh peers play
        // no part in the choice.
        builder.addClientServerChannel(TutorialNames.ticketingChannel)
          .client()
          .connect('tcp://127.0.0.1:7711');
        // --8<-- [end:clientserver-client-register]

        // --8<-- [start:fanout-publish-register]
        // The publisher keeps no subscriber list. Subscribers may come and go with
        // no change here. With a Location Store registered the publisher also has
        // to say which identity it publishes under, because the store keeps a row
        // per publisher; without one the routing id can be left out.
        builder.addFanoutChannel(TutorialNames.broadcastChannel)
          .setRoutingIdPrefix('game-client-broadcast')
          .enablePublisher('tcp://127.0.0.1:7712');
        // --8<-- [end:fanout-publish-register]

        // --8<-- [start:spot-client-register]
        // client() rules out registering factories. This node creates and calls
        // rooms; a node that picked server() runs them.
        mesh.objects().client();
        // --8<-- [end:spot-client-register]

        return builder.build();
      }
    })
  ]
})
class ClientModule {}

type RouteHandler = (params: readonly string[], body: string) => Promise<HttpResult>;

interface Route {
  readonly method: string;
  readonly pattern: RegExp;
  readonly handle: RouteHandler;
}

const routes: Route[] = [];

function map(method: string, pattern: RegExp, handle: RouteHandler): void {
  routes.push({ method, pattern, handle });
}

function ok(body: unknown): HttpResult {
  return { status: 200, body };
}

function accepted(): HttpResult {
  return { status: 202 };
}

function registerRoutes(
  route: ZLinkRouteClient,
  channels: ZLinkChannelClient,
  fanout: ZLinkFanoutClient,
  rooms: ZLinkSpotManager,
  spots: ZLinkSpotOutbound,
  playerManager: ZLinkActorManager,
  players: ZLinkActorClient
): void {
  // --8<-- [start:channel-request-call]
  map('GET', /^\/players\/([^/]+)\/profile$/, async ([playerId]) => {
    // The target is a channel name. Which node answers is decided at call time.
    const profile = await route
      .requestToChannel(TutorialNames.profileChannel, new GetPlayerProfile(playerId))
      .submit<PlayerProfile>();

    return ok(profile);
  });
  // --8<-- [end:channel-request-call]

  // --8<-- [start:channel-send-call]
  map('POST', /^\/players\/([^/]+)\/logins$/, async ([playerId]) => {
    // Returns as soon as the message is sent, with no reply to wait for.
    await route
      .sendToChannel(TutorialNames.profileChannel, new RecordLogin(playerId))
      .submit();

    return accepted();
  });
  // --8<-- [end:channel-send-call]

  // --8<-- [start:node-direct-call]
  map('GET', /^\/ops\/nodes\/([^/]+)\/status$/, async ([nodeRid]) => {
    // The target is one node, named by its routing id. No channel takes part,
    // so no candidate is chosen: this node answers or the call fails.
    const status = await route
      .requestToNode(TutorialNames.mesh, nodeRid, new GetNodeStatus())
      .submit<NodeStatus>();

    return ok(status);
  });
  // --8<-- [end:node-direct-call]

  // --8<-- [start:clientserver-call]
  map('POST', /^\/players\/([^/]+)\/tickets$/, async ([playerId]) => {
    // Same call shape as a mesh channel; only the routing differs, and a
    // ClientServer channel is reached through the channel client rather than
    // the route client.
    const ticket = await channels
      .requestToChannel(TutorialNames.ticketingChannel, new IssueSessionTicket(playerId))
      .submit<SessionTicket>();

    return ok(ticket.value);
  });
  // --8<-- [end:clientserver-call]

  // --8<-- [start:fanout-call]
  map('POST', /^\/notices$/, async (_params, body) => {
    const notice = JSON.parse(body) as { message: string };

    // Delivered to every subscriber. No recipient is named.
    await fanout
      .publish(TutorialNames.broadcastChannel, new MaintenanceNotice(notice.message))
      .submit();

    return accepted();
  });
  // --8<-- [end:fanout-call]

  // --8<-- [start:spot-create-call]
  map('POST', /^\/rooms$/, async (_params, body) => {
    const request = JSON.parse(body) as { title: string };

    const created = await rooms
      .create(TutorialNames.gameRoomType)   // Picks the factory and the candidate nodes.
      .inMesh(TutorialNames.mesh)
      .request(new OpenRoom(request.title)) // Reaches the room's create callback.
      .submit();

    // From here on the room is addressed by this id alone.
    return ok(created.spot.spotId);
  });
  // --8<-- [end:spot-create-call]

  // --8<-- [start:spot-message-call]
  // --8<-- [start:spot-send-call]
  map('POST', /^\/rooms\/([^/]+)\/chat$/, async ([roomId], body) => {
    const message = JSON.parse(body) as { playerId: string; text: string };

    // The id is enough; the Framework resolves where the room currently runs.
    await spots
      .sendToSpot(roomId, new PostChat(message.playerId, message.text))
      .submit();

    return accepted();
  });
  // --8<-- [end:spot-send-call]

  // --8<-- [start:spot-request-call]
  map('GET', /^\/rooms\/([^/]+)$/, async ([roomId]) => {
    const state = await spots
      .requestToSpot(roomId, new GetRoomState())
      .timeout(3000)
      .submit<RoomState>();

    return ok(state);
  });
  // --8<-- [end:spot-request-call]
  // --8<-- [end:spot-message-call]

  // --8<-- [start:location-find]
  // find answers from the Location Store alone: it reports where the object is,
  // and only while it is ready to receive. Nothing is sent to the object.
  map('GET', /^\/locations\/rooms\/([^/]+)$/, async ([roomId]) => {
    const room = await rooms.find(roomId);

    return room === undefined
      ? { status: 404 }
      : ok({ spotId: room.spotId, node: room.nodeRid });
  });

  map('GET', /^\/locations\/players\/([^/]+)$/, async ([playerId]) => {
    const player = await playerManager.find(playerId);

    return player === undefined
      ? { status: 404 }
      : ok({ actorId: player.actorId, node: player.nodeRid });
  });
  // --8<-- [end:location-find]

  // --8<-- [start:actor-create-call]
  map('POST', /^\/players\/([^/]+)$/, async ([playerId], body) => {
    const request = JSON.parse(body) as { nickname: string };

    // getOrCreate returns the existing player if there is one. The caller does
    // not choose which node hosts it.
    const result = await playerManager
      .getOrCreate(playerId, TutorialNames.playerActorType)
      .inMesh(TutorialNames.mesh)
      .request(new CreatePlayer(request.nickname))
      .timeout(10_000)
      .submit();

    return ok(result.status);
  });
  // --8<-- [end:actor-create-call]

  // --8<-- [start:actor-send-call]
  map('POST', /^\/players\/([^/]+)\/nickname$/, async ([playerId], body) => {
    const message = JSON.parse(body) as { nickname: string };

    // Addressed by player id, like a room is by room id.
    await players.sendToActor(playerId, new ChangeNickname(message.nickname)).submit();

    return accepted();
  });
  // --8<-- [end:actor-send-call]

  // --8<-- [start:actor-request-call]
  map('GET', /^\/players\/([^/]+)$/, async ([playerId]) => {
    const info = await players
      .requestToActor(playerId, new GetPlayer())
      .timeout(3000)
      .submit<PlayerInfo>();

    return ok(info);
  });
  // --8<-- [end:actor-request-call]
}

// This process's own HTTP surface -- a plain node:http server, not a NestJS
// HTTP module, the same shape the quickstart uses.
function startHttpServer(): http.Server {
  const server = http.createServer((request, response) => {
    const url = new URL(request.url ?? '/', 'http://127.0.0.1');
    const chunks: Buffer[] = [];
    request.on('data', (chunk: Buffer) => chunks.push(chunk));
    request.on('end', () => {
      const matched = routes
        .map((candidate) => ({ candidate, match: candidate.pattern.exec(url.pathname) }))
        .find((entry) => entry.match !== null && entry.candidate.method === request.method);
      if (matched === undefined || matched.match === null) {
        response.writeHead(404).end();
        return;
      }
      const params = matched.match.slice(1).map((value) => decodeURIComponent(value));
      // The error mapping sits here, ahead of every route registered above.
      withZLinkErrorResponse(() => matched.candidate
        .handle(params, Buffer.concat(chunks).toString('utf8')))
        .then((result) => {
          if (result.body === undefined) {
            response.writeHead(result.status).end();
            return;
          }
          response.writeHead(result.status, { 'content-type': 'application/json' });
          response.end(JSON.stringify(result.body));
        })
        .catch((error: unknown) => {
          response.writeHead(500, { 'content-type': 'application/json' });
          response.end(JSON.stringify({
            error: error instanceof Error ? error.message : String(error)
          }));
        });
    });
  });
  server.listen(5480, '127.0.0.1');
  return server;
}

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(ClientModule, { logger: ['error', 'warn', 'log'] });
  registerRoutes(
    app.get<ZLinkRouteClient>(ZLINK_ROUTE_CLIENT, { strict: false }),
    app.get<ZLinkChannelClient>(ZLINK_CHANNEL_CLIENT, { strict: false }),
    app.get<ZLinkFanoutClient>(ZLINK_FANOUT_CLIENT, { strict: false }),
    app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER, { strict: false }),
    app.get<ZLinkSpotOutbound>(ZLINK_SPOT_OUTBOUND, { strict: false }),
    app.get<ZLinkActorManager>(ZLINK_ACTOR_MANAGER, { strict: false }),
    app.get<ZLinkActorClient>(ZLINK_ACTOR_CLIENT, { strict: false })
  );
  startHttpServer();
  console.log('client listening on http://127.0.0.1:5480');
}

main().catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
