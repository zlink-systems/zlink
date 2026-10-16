import {
  ZLinkSpotCreateState,
  ZLinkUserSpotExecutionMode,
  type ZLinkSpotManager
} from '@zlink-systems/framework';
import { ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { Entry, Zone, ZoneRelocation } from './zone';
import { PlayerFactory, PlayerRelocation } from './player';
import {
  EnterWorld,
  Reconnect,
  Move,
  BorderTick,
  BorderHandler,
  BorderDelivery,
  OpenDungeon
} from './handlers';
runHost((_builder, config, mesh) => {
  if (!config.zoneIds || config.zoneCapacity === undefined)
    throw new Error('Zone configuration required');
  const objects = mesh.objects().server();
  objects.addEntrySpot(Entry);
  objects.addActorFactory(Names.actor, PlayerFactory, (f) => f.preserveStateWith(PlayerRelocation));
  objects.addSpotFactory(Names.zone, Zone, (f) => {
    f.stableTypeLimit(config.zoneCapacity!);
    f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    f.preserveStateWith(ZoneRelocation);
  });
  mesh.channel(Names.border).server();
  return {
    providers: [
      Entry,
      Zone,
      ZoneRelocation,
      PlayerFactory,
      PlayerRelocation,
      EnterWorld,
      Reconnect,
      Move,
      BorderTick,
      BorderHandler,
      BorderDelivery,
      OpenDungeon
    ],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable,
    async initialize(app) {
      if (!config.bootstrap) return;
      const spots = app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER);
      for (const id of config.zoneIds!) {
        const result = await spots.getOrCreate(id, Names.zone).inMesh(Names.mesh).submit();
        if (result.state === ZLinkSpotCreateState.Rejected)
          throw new Error(`Zone bootstrap rejected: ${id}`);
      }
    }
  };
}).catch(failHost);
