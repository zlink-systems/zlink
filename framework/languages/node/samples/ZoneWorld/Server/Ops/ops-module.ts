import { zlinkFramework, zlinkModule, ZLinkModule } from '@zlink-systems/nestjs';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZONEWORLD_CONFIG, createZoneWorldConfigurationModule } from '../Configuration/configuration';
import type { ZoneWorldConfiguration } from '../Configuration/configuration';
import { createZoneWorldLocationStore, zoneWorldLocationOptions } from '../Configuration/location-store';
import { NodeIds, ZoneWorldNames } from '../../Shared/spec';
import { NodeRegistry } from './node-registry';
import { OpsSessionFactory } from './ops-session';
import { OpsConsoleRegistry } from './ops-console-registry';
import { MaintenanceStore } from '../Configuration/maintenance-store';
import { OpsRuntimeStatusObserver } from './ops-runtime-events';

function createOpsModule() {
  class OpsModule {}
  const configuration = createZoneWorldConfigurationModule('ops');
  zlinkModule(__dirname, {
    imports: [configuration, ZLinkModule.forRootFactory({
      imports: [configuration],
      inject: [ZONEWORLD_CONFIG],
      useFactory: (value: unknown) => {
        const config = value as ZoneWorldConfiguration;
        const ops = config.ops;
        if (ops === undefined) throw new Error('Ops configuration is required.');
        const builder = zlinkFramework();
        builder.addLocationStore(createZoneWorldLocationStore(config.shared));
        zoneWorldLocationOptions(builder.configureLocations());
        builder.configureDispatch()
          .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
          .traceLogFile(`${config.shared.logDirectory}/flow-ops.log`)
          .traceLabel('ops');
        builder.addStreamNode(ZoneWorldNames.opsStreamNode)
          .bind(ops.streamEndpoint)
          .registerSession(OpsSessionFactory);
        builder.addFanoutChannel(ZoneWorldNames.broadcastChannel)
          .enablePublisher(ops.broadcastEndpoint)
          .setRoutingIdPrefix('zoneworld-ops');
        const mesh = builder.addRouteMesh(ZoneWorldNames.zoneMesh)
          .listen(ops.reportEndpoint);
        mesh.channel(ZoneWorldNames.reportChannel).server().addHandlerGroup('ops');
        mesh.channel(ZoneWorldNames.zoneMesh).client();
        mesh.channel(ZoneWorldNames.bridgeMesh).client();
        for (const nodeId of [NodeIds.west, NodeIds.east]) {
          const channelName = ZoneWorldNames.opsChannel(nodeId);
          mesh.channel(channelName).client();
        }
        return builder.build();
      }
    })],
    providers: [
      NodeRegistry,
      MaintenanceStore,
      OpsConsoleRegistry,
      OpsSessionFactory,
      OpsRuntimeStatusObserver
    ]
  })(OpsModule);
  return OpsModule;
}

export { createOpsModule };
