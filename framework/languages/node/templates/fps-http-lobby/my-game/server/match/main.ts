import { ZLinkUserSpotExecutionMode } from '@zlink-systems/framework';
import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { Entry, Room, RoomRelocation } from './spots';
import { PlayerFactory, PlayerRelocation } from './player';
import { Prepare, JoinRoom, Reconnect, View, Move, Shoot, Delivery } from './handlers';
runHost((_builder, config, mesh) => {
  const capacity = config.roomCapacity;
  if (capacity === undefined || !Number.isSafeInteger(capacity) || capacity < 1)
    throw new Error('Positive Room capacity required');
  const objects = mesh.objects().server();
  objects.addEntrySpot(Entry, Names.actor);
  objects.addActorFactory(Names.actor, PlayerFactory, (f) => f.preserveStateWith(PlayerRelocation));
  objects.addSpotFactory(Names.room, Room, (f) => {
    f.stableTypeLimit(capacity);
    f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    f.preserveStateWith(RoomRelocation);
  });
  return {
    providers: [
      Entry,
      Room,
      RoomRelocation,
      PlayerFactory,
      PlayerRelocation,
      Prepare,
      JoinRoom,
      Reconnect,
      View,
      Move,
      Shoot,
      Delivery
    ],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable
  };
}).catch(failHost);
