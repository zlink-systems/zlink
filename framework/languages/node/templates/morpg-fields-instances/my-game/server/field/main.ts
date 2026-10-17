import {
  ZLinkSpotCreateState,
  ZLinkUserSpotExecutionMode,
  type ZLinkSpotManager
} from '@zlink-systems/framework';
import { ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { Entry, EnterWorld } from '../shared/entry';
import { PlayerFactory, PlayerRelocation } from '../shared/player';
import { Field, FieldRelocation } from './spots';
import {
  DepartedDungeonView,
  Reconnect,
  Move,
  CreateParty,
  JoinParty,
  EnterDungeon,
  DungeonTravel,
  BorderTick,
  BorderHandler,
  Delivery
} from './handlers';
runHost((_builder, config, mesh) => {
  const objects = mesh.objects().server();
  objects.addEntrySpot(Entry);
  objects.addActorFactory(Names.actor, PlayerFactory, (f) => f.preserveStateWith(PlayerRelocation));
  objects.addSpotFactory(Names.field, Field, (f) => {
    f.stableTypeLimit(config.fieldCapacity);
    f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    f.preserveStateWith(FieldRelocation);
  });
  mesh.channel(Names.border).server();
  return {
    providers: [
      DepartedDungeonView,
      Entry,
      EnterWorld,
      Field,
      FieldRelocation,
      PlayerFactory,
      PlayerRelocation,
      Reconnect,
      Move,
      CreateParty,
      JoinParty,
      EnterDungeon,
      DungeonTravel,
      BorderTick,
      BorderHandler,
      Delivery
    ],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable,
    async initialize(app) {
      for (const id of config.fieldIds) {
        const result = await app
          .get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER)
          .getOrCreate(id, Names.field)
          .inMesh(Names.mesh)
          .submit();
        if (result.state === ZLinkSpotCreateState.Rejected)
          throw new Error('Field bootstrap rejected');
      }
    }
  };
}).catch(failHost);
