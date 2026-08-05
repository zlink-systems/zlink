import { zlinkFramework, zlinkModule, ZLinkModule } from '@zlink-systems/nestjs';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZONEWORLD_CONFIG, createZoneWorldConfigurationModule } from '../Configuration/configuration';
import type { ZoneWorldConfiguration } from '../Configuration/configuration';
import { createZoneWorldLocationStore, zoneWorldLocationOptions } from '../Configuration/location-store';
import { ZoneWorldNames } from '../../Shared/spec';
import { PlayerSessionFactory } from './player-session';
import { GatewaySpotEventHandler } from './gateway-runtime-events';

function createGatewayModule() {
  class GatewayModule {}
  const configuration = createZoneWorldConfigurationModule('gateway');
  zlinkModule(__dirname, {
    imports: [configuration, ZLinkModule.forRootFactory({
      imports: [configuration],
      inject: [ZONEWORLD_CONFIG],
      useFactory: (value: unknown) => {
        const config = value as ZoneWorldConfiguration;
        const gateway = config.gateway;
        if (gateway === undefined) throw new Error('Gateway configuration is required.');
        const builder = zlinkFramework();
        builder.addLocationStore(createZoneWorldLocationStore(config.shared));
        zoneWorldLocationOptions(builder.configureLocations());
        builder.configureDispatch()
          .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
          .traceLogFile(`${config.shared.logDirectory}/flow-gateway.log`)
          .traceLabel('gateway');
        const zoneMesh = builder
          .addRouteMesh(ZoneWorldNames.zoneMesh)
            .setRoutingIdPrefix('gw0')
            .listen(gateway.spotRouterEndpoint);
        zoneMesh.channel(ZoneWorldNames.zoneMesh).client();
        zoneMesh.channel(ZoneWorldNames.bridgeMesh).client();
        zoneMesh.objects().client();
        zoneMesh.channel(ZoneWorldNames.reportChannel).client();
        for (const nodeId of ['zone-node-1', 'zone-node-2']) {
          zoneMesh.channel(ZoneWorldNames.opsChannel(nodeId)).client();
        }
        builder.addStreamNode(ZoneWorldNames.gatewayStreamNode)
          .enableActorDispatch()
          .bind(gateway.streamEndpoint)
          .registerSession(PlayerSessionFactory);
        const registration = builder.build();
        return {
          ...registration,
          monitoring: {}
        };
      }
    })],
    providers: [GatewaySpotEventHandler, PlayerSessionFactory]
  })(GatewayModule);
  return GatewayModule;
}

export { createGatewayModule };
