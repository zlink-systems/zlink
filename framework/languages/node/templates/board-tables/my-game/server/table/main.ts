import { addGameMesh, runHost, failHost } from '../shared/hosting';
import { Limits, Names } from '../shared/constants';
import { ZLINK_CLIENT_SERVER_RUNTIME } from '@zlink-systems/nestjs';
import { PlayerRecords } from './application/player-records';
import { PlayerRecordsChannel } from './infrastructure/player-records-channel';
import {
  ZLinkUserSpotExecutionMode,
  type ZLinkClientServerRuntime
} from '@zlink-systems/framework';
import { Entry, Table, TableRelocation } from './infrastructure/table';
import { PlayerFactory, PlayerRelocation } from './infrastructure/player';
import { EntryJoin, Rejoin, View, Start, Bet, Deliver } from './infrastructure/handlers';
import {
  AssignMatchHandler,
  CancelMatchHandler,
  CommitRoomHandler,
  MatchCommittedHandler,
  MatchCancelledHandler,
  FindMatchHandler,
  PrepareRoomHandler,
  ReleaseMatchHandler,
  WithdrawRoomHandler
} from './infrastructure/match-handlers';
runHost((builder, config) => {
  const mesh = addGameMesh(builder, config);
  builder.addClientServerChannel(Names.playerRecords).client();
  const objects = mesh.objects().server();
  objects.addEntrySpot(Entry, Names.actor);
  objects.addActorFactory(Names.actor, PlayerFactory, (f) => f.preserveStateWith(PlayerRelocation));
  objects.addSpotFactory(Names.table, Table, (f) => {
    if (config.tableCapacity) f.stableTypeLimit(config.tableCapacity);
    f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    f.preserveStateWith(TableRelocation);
  });
  return {
    providers: [
      { provide: PlayerRecords, useClass: PlayerRecordsChannel },
      Entry,
      Table,
      TableRelocation,
      PlayerFactory,
      PlayerRelocation,
      EntryJoin,
      Rejoin,
      View,
      Start,
      Bet,
      Deliver,
      FindMatchHandler,
      CancelMatchHandler,
      AssignMatchHandler,
      MatchCancelledHandler,
      ReleaseMatchHandler,
      PrepareRoomHandler,
      CommitRoomHandler,
      MatchCommittedHandler,
      WithdrawRoomHandler
    ],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable,
    async initialize(app) {
      const channel = app.get<ZLinkClientServerRuntime>(ZLINK_CLIENT_SERVER_RUNTIME);
      for await (const observed of channel.observe(
        Names.playerRecords,
        Limits.observationCapacity,
        AbortSignal.timeout(Limits.startupMs)
      )) {
        if (observed.loss.discardedTerminalCount !== 0n)
          throw new Error('Player records readiness observation lost');
        if (observed.status.isReady) return;
      }
      throw new Error('Player records channel did not become ready');
    }
  };
}).catch(failHost);
