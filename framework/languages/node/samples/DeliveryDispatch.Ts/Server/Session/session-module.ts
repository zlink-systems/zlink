import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { SampleNames } from '../../Shared/Configuration/sample-names';
import { CustomerSessionFactory } from './customer-session';
import { CustomerActorDirectory, CustomerActorFactory } from './customer-actor';
import { CustomerEntrySpot } from './customer-entry-spot';
import { createDeliveryDispatchLocationStore, deliveryDispatchLocationOptions } from '../Configuration/location-store';
import {
  DELIVERYDISPATCH_SAMPLE_CONFIG,
  createDeliveryDispatchConfigurationModule
} from '../Configuration/sample-config';
import type { DeliveryDispatchServerConfig } from '../Configuration/sample-config';

function createSessionModule() {
  class SessionModule {}
  const directory = new CustomerActorDirectory();
  CustomerActorFactory.useDirectory(directory);
  const configuration = createDeliveryDispatchConfigurationModule([
    'sessionSpotRouterEndpoint',
    'sessionStreamEndpoint',
    'redisEndpoint',
    'redisKeyPrefix',
    'logDir'
  ]);

  zlinkModule(__dirname, {
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [DELIVERYDISPATCH_SAMPLE_CONFIG],
        useFactory: (config: DeliveryDispatchServerConfig) => {
          const builder = zlinkFramework();
          builder.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
            .traceLogFile(`${config.logDir}/flow-customer-gateway.log`)
            .traceLabel('customer-gateway');
          builder.addLocationStore(createDeliveryDispatchLocationStore(config));
          deliveryDispatchLocationOptions(builder.configureLocations());
          const mesh = builder.addRouteMesh(SampleNames.customerMeshName)
              .listen(config.sessionSpotRouterEndpoint).setRoutingIdPrefix('delivery-customer');
          const objectServer = mesh.objects().server();
          objectServer.addEntrySpot(CustomerEntrySpot);
          objectServer.addActorFactory(
            SampleNames.customerActorType,
            CustomerActorFactory,
            (factory) => factory.disableRelocation()
          );
          return builder.addStreamNode(SampleNames.customerStreamNode)
              .enableActorDispatch()
              .bind(config.sessionStreamEndpoint)
              .registerSession(CustomerSessionFactory)
            .build();
        }
      })
    ],
    providers: [
      { provide: CustomerActorDirectory, useValue: directory },
      {
        provide: 'DELIVERYDISPATCH_LOCATION_STORE',
        inject: [DELIVERYDISPATCH_SAMPLE_CONFIG],
        useFactory: (config: DeliveryDispatchServerConfig) => createDeliveryDispatchLocationStore(config)
      },
      CustomerSessionFactory,
      CustomerActorFactory,
      CustomerEntrySpot
    ]
  })(SessionModule);

  return SessionModule;
}

export { createSessionModule };
