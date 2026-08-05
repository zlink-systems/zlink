import { zlinkFramework, zlinkModule, ZLinkModule } from '@zlink-systems/nestjs';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import {
  ZONEWORLD_CONFIG,
  createZoneWorldConfigurationModule
} from '../Configuration/configuration';
import type { ZoneWorldConfiguration } from '../Configuration/configuration';
import {
  createZoneWorldLocationStore,
  createZoneWorldRelocationStore,
  zoneWorldLocationOptions
} from '../Configuration/location-store';
import { ZoneWorldNames, zonesOf } from '../../Shared/spec';
import { PlayerActorFactory } from './Infrastructure/ZLink/Actors/player-actor-factory';
import { PlayerActorRelocationAdapter } from './Infrastructure/ZLink/Actors/player-actor-relocation-adapter';
import { ZoneEntrySpot } from './Infrastructure/ZLink/Spots/zone-entry-spot';
import { ZoneSpot } from './Infrastructure/ZLink/Spots/zone-spot';
import {
  PlayerMovement
} from './Infrastructure/ZLink/Handlers/player-handlers';
import {
  MaintenanceChangedSubscriber,
  WorldAnnounceSubscriber
} from './Infrastructure/ZLink/Handlers/node-channel-handlers';
import { MaintenanceStore } from '../Configuration/maintenance-store';
import { NodeRuntimeState } from './Domain/node-runtime-state';
import { SpotRuntimeStatusObserver } from './Infrastructure/ZLink/Handlers/spot-runtime-event-handler';
import { OpsReportAdapter } from './Infrastructure/ZLink/Monitoring/ops-report-adapter';


function createZoneNodeModule(includeZoneRuntime = true) {
  class ZoneNodeModule {}
  const configuration = createZoneWorldConfigurationModule('zoneNode');
  zlinkModule({
    providerDiscovery: includeZoneRuntime
      ? [{ rootDir: __dirname, options: { recursive: true } }]
      : [],
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [ZONEWORLD_CONFIG],
        useFactory: (value: unknown) => {
          const config = value as ZoneWorldConfiguration;
          const node = config.zoneNode;
          if (node === undefined) throw new Error('ZoneNode configuration is required.');
          const builder = zlinkFramework();
          builder.addLocationStore(createZoneWorldLocationStore(config.shared));
          builder.addRelocationStore(createZoneWorldRelocationStore(config.shared));
          zoneWorldLocationOptions(builder.configureLocations());
          builder.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
            .traceLogFile(`${config.shared.logDirectory}/flow-${node.nodeId}.log`)
            .traceLabel(node.nodeId);

          if (zonesOf(node.nodeId).length === 0) {
            builder.addFanoutChannel(ZoneWorldNames.broadcastChannel)
              .enableSubscriber()
              .addHandlerGroup('zone-broadcast');
            return builder.build();
          }

          const zoneMesh = builder.addRouteMesh(ZoneWorldNames.zoneMesh)
            .setRoutingIdPrefix('zn')
            .listen(node.spotRouterEndpoint);
          const objectServer = zoneMesh.objects().server();
          objectServer.addEntrySpot(ZoneEntrySpot);
          objectServer.addSpotFactory(
            ZoneSpot.name,
            ZoneSpot,
            (factory) => factory.disableRelocation()
          );
          objectServer.addActorFactory(
            ZoneWorldNames.playerActorType,
            PlayerActorFactory,
            (factory) => factory.preserveStateWith(PlayerActorRelocationAdapter)
          );
          zoneMesh.channel(ZoneWorldNames.zoneMesh).server();
          zoneMesh.channel(ZoneWorldNames.bridgeMesh).server();
          zoneMesh.channel(ZoneWorldNames.reportChannel).client();
          const opsChannelName = ZoneWorldNames.opsChannel(node.nodeId);
          for (const configuredNodeId of ['zone-node-1', 'zone-node-2']) {
            const configuredChannel = ZoneWorldNames.opsChannel(configuredNodeId);
            const membership = zoneMesh.channel(configuredChannel).server();
            if (configuredChannel === opsChannelName) membership.addHandlerGroup('zone-ops');
            else membership.setWeight(0);
          }
          builder.addFanoutChannel(ZoneWorldNames.broadcastChannel)
            .enableSubscriber()
            .addHandlerGroup('zone-broadcast');
          return {
            ...builder.build(),
            monitoring: {}
          };
        }
      })
    ],
    providers: [
      MaintenanceStore,
      NodeRuntimeState,
      ...(includeZoneRuntime ? [
        PlayerActorFactory,
        PlayerActorRelocationAdapter,
        ZoneEntrySpot,
        ZoneSpot,
        PlayerMovement,
        SpotRuntimeStatusObserver,
        OpsReportAdapter
      ] : [
        WorldAnnounceSubscriber,
        MaintenanceChangedSubscriber
      ])
    ]
  })(ZoneNodeModule);
  return ZoneNodeModule;
}

export { createZoneNodeModule };
