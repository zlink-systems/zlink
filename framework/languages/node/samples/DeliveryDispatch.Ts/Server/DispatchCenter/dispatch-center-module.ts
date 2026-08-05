import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { SampleNames } from '../../Shared/Configuration/sample-names';
import { DispatchWorker } from './dispatch-worker';
import { DeliveryOfferStore } from './delivery-offer-store';
import { createDeliveryDispatchLocationStore, deliveryDispatchLocationOptions } from '../Configuration/location-store';
import {
  DELIVERYDISPATCH_SAMPLE_CONFIG,
  createDeliveryDispatchConfigurationModule
} from '../Configuration/sample-config';
import type { DeliveryDispatchServerConfig } from '../Configuration/sample-config';

function createDispatchCenterModule() {
  class DispatchCenterModule {}
  const configuration = createDeliveryDispatchConfigurationModule([
    'dispatchEndpoint',
    'dispatchSpotEndpoint',
    'redisEndpoint',
    'redisKeyPrefix',
    'logDir',
    'workDir'
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
            .traceLogFile(`${config.logDir}/flow-dispatch-center.log`)
            .traceLabel('dispatch-center');
          builder.addLocationStore(createDeliveryDispatchLocationStore(config));
          deliveryDispatchLocationOptions(builder.configureLocations());
          const mesh = builder.addRouteMesh(SampleNames.courierMeshName)
            .listen(config.dispatchSpotEndpoint).setRoutingIdPrefix('delivery-dispatch');
          mesh.objects().client();
          const dispatchChannel = builder.addClientServerChannel(SampleNames.dispatchChannel);
          dispatchChannel.client();
          dispatchChannel
            .server()
            .listen()
            .addHandlerGroup('dispatch');
          builder.addClientServerChannel(SampleNames.trackingChannel).client();
          return builder.build();
        }
      })
    ],
    providers: [
      {
        provide: 'DELIVERYDISPATCH_LOCATION_STORE',
        inject: [DELIVERYDISPATCH_SAMPLE_CONFIG],
        useFactory: (config: DeliveryDispatchServerConfig) => createDeliveryDispatchLocationStore(config)
      },
      {
        provide: DeliveryOfferStore,
        inject: [DELIVERYDISPATCH_SAMPLE_CONFIG],
        useFactory: (config: DeliveryDispatchServerConfig) => new DeliveryOfferStore(config.workDir)
      },
      DispatchWorker
    ]
  })(DispatchCenterModule);

  return DispatchCenterModule;
}

export { createDispatchCenterModule };
