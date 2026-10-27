import { ZLinkUserSpotExecutionMode } from '@zlink-systems/framework';
import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { Dungeon, DungeonInfo } from './infrastructure/zlink/spots';
runHost((_builder, _config, mesh) => {
  mesh
    .objects()
    .server()
    .addSpotFactory(Names.dungeon, Dungeon, (f) => {
      f.recreateOnRelocation();
      f.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
    });
  return {
    providers: [Dungeon, DungeonInfo],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable
  };
}).catch(failHost);
