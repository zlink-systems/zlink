import {
  ZLinkSpotCreateState,
  ZLinkUserSpotExecutionMode,
  type ZLinkSpotManager
} from '@zlink-systems/framework';
import { ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { PlayerFactory, PlayerRelocation } from '../shared/player';
import { Town, TownRelocation } from './town';
import { Entry, EnterTown } from '../shared/entry';
import {
  TownQuery,
  TownReconnect,
  ChangeChannel,
  CreateParty,
  JoinParty,
  EnterDungeon,
  DungeonTravel,
  TownDelivery
} from './handlers';
runHost((_builder, config, mesh) => {
  if (!config.townId || !Number.isInteger(config.townCapacity) || config.townCapacity! < 1)
    throw new Error('Town configuration required');
  const objects = mesh.objects().server();
  objects.addEntrySpot(Entry, Names.actor);
  objects.addActorFactory(Names.actor, PlayerFactory, (f) => f.preserveStateWith(PlayerRelocation));
  objects.addSpotFactory(Names.town, Town, (f) => {
    f.stableTypeLimit(config.townCapacity!);
    f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    f.preserveStateWith(TownRelocation);
  });
  return {
    providers: [
      Entry,
      Town,
      TownRelocation,
      PlayerFactory,
      PlayerRelocation,
      EnterTown,
      TownQuery,
      TownReconnect,
      ChangeChannel,
      CreateParty,
      JoinParty,
      EnterDungeon,
      DungeonTravel,
      TownDelivery
    ],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable,
    async initialize(app) {
      if (!config.bootstrap) return;
      const result = await app
        .get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER)
        .getOrCreate(config.townId!, Names.town)
        .inMesh(Names.mesh)
        .submit();
      if (result.state === ZLinkSpotCreateState.Rejected)
        throw new Error('Town bootstrap rejected');
    }
  };
}).catch(failHost);
