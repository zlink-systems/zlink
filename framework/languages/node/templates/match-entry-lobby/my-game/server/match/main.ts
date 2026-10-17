import {
  ZLinkSpotCreateState,
  ZLinkUserSpotExecutionMode,
  type ZLinkSpotManager
} from '@zlink-systems/framework';
import { ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { Entry, Matchmaking, MatchmakingRelocation, Room, RoomRelocation } from './spots';
import { PlayerFactory, PlayerRelocation } from './player';
import {
  EnterQueue,
  QueueAgain,
  Assignment,
  Reconnect,
  View,
  Move,
  Shoot,
  Delivery
} from './handlers';
runHost((_builder, config, mesh) => {
  const objects = mesh.objects().server();
  objects.addEntrySpot(Entry, Names.actor);
  objects.addActorFactory(Names.actor, PlayerFactory, (f) => f.preserveStateWith(PlayerRelocation));
  objects.addSpotFactory(Names.matchmaking, Matchmaking, (f) => {
    f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    f.preserveStateWith(MatchmakingRelocation);
  });
  objects.addSpotFactory(Names.room, Room, (f) => {
    f.stableTypeLimit(config.roomCapacity);
    f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    f.preserveStateWith(RoomRelocation);
  });
  return {
    providers: [
      Entry,
      Matchmaking,
      MatchmakingRelocation,
      Room,
      RoomRelocation,
      PlayerFactory,
      PlayerRelocation,
      EnterQueue,
      QueueAgain,
      Assignment,
      Reconnect,
      View,
      Move,
      Shoot,
      Delivery
    ],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable,
    async initialize(app) {
      if (!config.bootstrapMatchmaking) return;
      const result = await app
        .get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER)
        .getOrCreate(Names.matchmakingId, Names.matchmaking)
        .inMesh(Names.mesh)
        .submit();
      if (result.state === ZLinkSpotCreateState.Rejected)
        throw new Error('Matchmaking bootstrap rejected');
    }
  };
}).catch(failHost);
