import {
  ZLinkSpotCreateState,
  ZLinkUserSpotExecutionMode,
  type ZLinkSpotManager
} from '@zlink-systems/framework';
import { ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import { runHost, failHost } from '../shared/hosting';
import { Names, Game, zoneId, meshName } from '../shared/constants';
import { PlayerData } from './data';
import { PlayerFactory, PlayerRelocation } from './player';
import { Entry, Zone, ZoneRelocation } from './spots';
import {
  EnterPlayer,
  EnterZone,
  ChangeChannel,
  EarnGold,
  ViewPlayer,
  ViewChannel,
  DeliverView,
  DeliverChannelView
} from './handlers';
runHost((_builder, config, mesh) => {
  const objects = mesh.objects().server();
  objects.addEntrySpot(Entry, Names.actor);
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
      ViewChannel,
      DeliverView,
      DeliverChannelView
    ],
    listener: { kind: 'routeMesh', name: meshName(config.worldId) },
    ready: (status) => status.placement.isAvailable,
    async initialize(app) {
      for (const id of config.zoneIds)
        await createZone(app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER), id);
    },
    async createUpdateZone(app) {
      await createZone(app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER), zoneId(Game.updateChannel));
    }
  };
  async function createZone(spots: ZLinkSpotManager, id: string): Promise<void> {
    const result = await spots
      .getOrCreate(id, Names.zone)
      .inMesh(meshName(config.worldId))
      .submit();
    if (result.state === ZLinkSpotCreateState.Rejected)
      throw new Error(`Zone creation rejected: ${id}`);
  }
}).catch(failHost);
