import { ZLinkUserSpotExecutionMode } from '@zlink-systems/framework';
import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { PlayerFactory, PlayerRelocation } from '../shared/player';
import { Dungeon, DungeonRelocation } from './dungeon';
import { Prepare, DungeonQuery, DungeonReconnect, Advance, DungeonDelivery } from './handlers';
runHost((_builder, config, mesh) => {
  if (!Number.isInteger(config.dungeonCapacity) || config.dungeonCapacity! < 1)
    throw new Error('Dungeon capacity required');
  const objects = mesh.objects().server();
  objects.addActorFactory(Names.actor, PlayerFactory, (f) => f.preserveStateWith(PlayerRelocation));
  objects.addSpotFactory(Names.dungeon, Dungeon, (f) => {
    f.stableTypeLimit(config.dungeonCapacity!);
    f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    f.preserveStateWith(DungeonRelocation);
  });
  return {
    providers: [
      PlayerFactory,
      PlayerRelocation,
      Dungeon,
      DungeonRelocation,
      Prepare,
      DungeonQuery,
      DungeonReconnect,
      Advance,
      DungeonDelivery
    ],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable
  };
}).catch(failHost);
