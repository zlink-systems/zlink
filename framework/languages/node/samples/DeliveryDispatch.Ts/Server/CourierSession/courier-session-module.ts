import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { SampleNames } from '../../Shared/Configuration/sample-names';
import { CourierSessionFactory } from './courier-session';
import { createDeliveryDispatchLocationStore, deliveryDispatchLocationOptions } from '../Configuration/location-store';
import {
  DELIVERYDISPATCH_SAMPLE_CONFIG,
  createDeliveryDispatchConfigurationModule
} from '../Configuration/sample-config';
import type { DeliveryDispatchServerConfig } from '../Configuration/sample-config';

function createCourierSessionModule() {
  class CourierSessionModule {}
  const configuration = createDeliveryDispatchConfigurationModule([
    'courierStreamEndpoint',
    'courierSessionSpotEndpoint',
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
            .traceLogFile(`${config.logDir}/flow-courier-session.log`)
            .traceLabel('courier-session');
          builder.addLocationStore(createDeliveryDispatchLocationStore(config));
          deliveryDispatchLocationOptions(builder.configureLocations());
          const mesh = builder.addRouteMesh(SampleNames.courierMeshName)
            .listen(config.courierSessionSpotEndpoint).setRoutingIdPrefix('delivery-session');
          mesh.objects().client();
          return builder.addStreamNode(SampleNames.courierStreamNode)
              .enableActorDispatch()
              .bind(config.courierStreamEndpoint)
              .registerSession(CourierSessionFactory)
            .build();
        }
      })
    ],
    providers: [
      {
        provide: 'DELIVERYDISPATCH_LOCATION_STORE',
        inject: [DELIVERYDISPATCH_SAMPLE_CONFIG],
        useFactory: (config: DeliveryDispatchServerConfig) => createDeliveryDispatchLocationStore(config)
      },
      CourierSessionFactory
    ]
  })(CourierSessionModule);

  return CourierSessionModule;
}

export { createCourierSessionModule };
