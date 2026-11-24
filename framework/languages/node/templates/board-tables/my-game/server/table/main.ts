import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { ZLinkUserSpotExecutionMode } from '@zlink-systems/framework';
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
runHost((_builder, config, mesh) => {
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
    ready: (status) => status.placement.isAvailable
  };
}).catch(failHost);
