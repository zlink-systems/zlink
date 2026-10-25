import { ZLinkUserSpotExecutionMode } from '@zlink-systems/framework';
import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { Entry, EnterWorld } from '../shared/entry';
import { PlayerFactory, PlayerRelocation } from '../shared/player';
import { Dungeon, DungeonRelocation } from './spots';
import { Reconnect, View, Advance, Prepare, Delivery, EnterAgain, TravelAgain } from './handlers';
runHost((_builder, config, mesh) => {
  const objects = mesh.objects().server();
  objects.addEntrySpot(Entry, Names.actor);
  objects.addActorFactory(Names.actor, PlayerFactory, (f) => f.preserveStateWith(PlayerRelocation));
  objects.addSpotFactory(Names.dungeon, Dungeon, (f) => {
    f.stableTypeLimit(config.dungeonCapacity);
    f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    f.preserveStateWith(DungeonRelocation);
  });
  return {
    providers: [EnterAgain, TravelAgain,
      Entry,
      EnterWorld,
      Dungeon,
      DungeonRelocation,
      PlayerFactory,
      PlayerRelocation,
      Reconnect,
      View,
      Advance,
      Prepare,
      Delivery
    ],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable
  };
}).catch(failHost);
