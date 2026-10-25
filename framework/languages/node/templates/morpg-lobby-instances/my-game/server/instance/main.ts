import { ZLinkUserSpotExecutionMode } from '@zlink-systems/framework';
import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { PlayerFactory, PlayerRelocation } from '../shared/player';
import { Entry } from '../shared/entry';
import { Login, CreateParty, JoinParty } from '../shared/entry-handlers';
import { Dungeon, DungeonRelocation } from './dungeon';
import { Prepare, Reconnect, View, Advance, Delivery, Enter, Travel, PartyDelivery, RejectDelivery } from './handlers';
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
    providers: [
      Entry,
      Login,
      CreateParty,
      JoinParty,
      Dungeon,
      DungeonRelocation,
      PlayerFactory,
      PlayerRelocation,
      Prepare,
      Reconnect,
      View,
      Advance,
      Delivery, Enter, Travel, PartyDelivery, RejectDelivery
    ],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable
  };
}).catch(failHost);
