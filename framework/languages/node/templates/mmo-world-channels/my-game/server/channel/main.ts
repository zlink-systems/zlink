import {
  ZLinkSpotCreateState,
  ZLinkUserSpotExecutionMode,
  type ZLinkSpotManager
} from '@zlink-systems/framework';
import { ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import { runHost, failHost } from '../shared/hosting';
import { Names, meshName } from '../shared/constants';
import { PlayerData } from './data';
import { PlayerFactory, PlayerRelocation } from './player';
import { Entry, Zone, ZoneRelocation } from './spots';
import {
  EnterPlayer,
  EnterZone,
  ChangeChannel,
  EarnGold,
  ViewPlayer,
  DeliverView
} from './handlers';
runHost((_builder, config, mesh) => {
  const objects = mesh.objects().server();
  objects.addEntrySpot(Entry);
  objects.addActorFactory(Names.actor, PlayerFactory, (f) => f.preserveStateWith(PlayerRelocation));
  objects.addSpotFactory(Names.zone, Zone, (f) => {
    f.stableTypeLimit(config.zoneCapacity);
    f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    f.preserveStateWith(ZoneRelocation);
  });
  return {
    providers: [
      PlayerData,
      PlayerFactory,
      PlayerRelocation,
      Entry,
      Zone,
      ZoneRelocation,
      EnterPlayer,
      EnterZone,
      ChangeChannel,
      EarnGold,
      ViewPlayer,
      DeliverView
    ],
    listener: { kind: 'routeMesh', name: meshName(config.worldId) },
    ready: (status) => status.placement.isAvailable,
    async initialize(app) {
      for (const id of config.zoneIds) {
        const result = await app
          .get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER)
          .getOrCreate(id, Names.zone)
          .inMesh(meshName(config.worldId))
          .submit();
        if (result.state === ZLinkSpotCreateState.Rejected)
          throw new Error(`Zone bootstrap rejected: ${id}`);
      }
    }
  };
}).catch(failHost);
