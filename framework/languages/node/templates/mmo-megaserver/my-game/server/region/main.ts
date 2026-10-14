import { ZLinkUserSpotExecutionMode } from '@zlink-systems/framework';
import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { Entry, Region, RegionRelocation } from './infrastructure/zlink/spots';
import { PlayerFactory, PlayerRelocation } from './infrastructure/zlink/player';
import {
  EnterRegion,
  NextRegion,
  ReconnectRegion,
  Move,
  View,
  OpenDungeon,
  Delivery
} from './infrastructure/zlink/handlers';
runHost((_builder, config, mesh) => {
  const objects = mesh.objects().server();
  objects.addEntrySpot(Entry);
  objects.addActorFactory(Names.actor, PlayerFactory, (f) => f.preserveStateWith(PlayerRelocation));
  objects.addSpotFactory(Names.region, Region, (f) => {
    f.stableTypeLimit(config.regionLimit);
    f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    f.preserveStateWith(RegionRelocation);
  });
  return {
    providers: [
      Entry,
      Region,
      RegionRelocation,
      PlayerFactory,
      PlayerRelocation,
      EnterRegion,
      NextRegion,
      ReconnectRegion,
      Move,
      View,
      OpenDungeon,
      Delivery
    ],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable
  };
}).catch(failHost);
