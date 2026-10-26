import { ZLinkUserSpotExecutionMode } from '@zlink-systems/framework';
import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { PlayerFactory, PlayerRelocation } from '../shared/player';
import { Party, PartyRelocation } from './spots';
import { Entry } from '../shared/entry';
import { EntryNotifications, Login, CreateParty, JoinParty } from '../shared/entry-handlers';
import { PartyNotifications, Reconnect, View, Ready, EnterDungeon, Travel, Delivery, Arrived, Started, Completed, EnterRun, RejectDelivery } from './handlers';
runHost((_builder, _config, mesh) => {
  const objects = mesh.objects().server();
  objects.addEntrySpot(Entry, Names.actor);
  objects.addActorFactory(Names.actor, PlayerFactory, (f) => f.preserveStateWith(PlayerRelocation));
  objects.addSpotFactory(Names.party, Party, (f) => {
    f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    f.preserveStateWith(PartyRelocation);
  });
  return {
    providers: [
      EntryNotifications,
      PartyNotifications,
      Entry,
      Party,
      PartyRelocation,
      PlayerFactory,
      PlayerRelocation,
      Login,
      CreateParty,
      JoinParty,
      Reconnect,
      View,
      Ready,
      EnterDungeon,
      Travel,
      Delivery, Arrived, Started, Completed, EnterRun, RejectDelivery
    ],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable
  };
}).catch(failHost);
