import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { SampleNames } from '../../Shared/Configuration/sample-names';
import { EvidenceStore } from '../Configuration/evidence-store';
import { createDeliveryDispatchLocationStore, deliveryDispatchLocationOptions } from '../Configuration/location-store';
import {
  DELIVERYDISPATCH_SAMPLE_CONFIG,
  createDeliveryDispatchConfigurationModule
} from '../Configuration/sample-config';
import type { DeliveryDispatchServerConfig } from '../Configuration/sample-config';

function createTrackingModule() {
  class TrackingModule {}
  const configuration = createDeliveryDispatchConfigurationModule([
    'trackingEndpoint',
    'trackingSpotEndpoint',
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
            .traceLogFile(`${config.logDir}/flow-tracking.log`)
            .traceLabel('tracking');
          builder.addLocationStore(createDeliveryDispatchLocationStore(config));
          deliveryDispatchLocationOptions(builder.configureLocations());
          const mesh = builder.addRouteMesh(SampleNames.customerMeshName)
            .listen(config.trackingSpotEndpoint)
            .setRoutingIdPrefix('delivery-tracking');
          mesh.objects().client();
          builder.addClientServerChannel(SampleNames.trackingChannel)
            .server()
            .listen()
            .addHandlerGroup('tracking');
          return builder.build();
        }
      })
    ],
    providers: [
      {
        provide: EvidenceStore,
        inject: [DELIVERYDISPATCH_SAMPLE_CONFIG],
        useFactory: (config: DeliveryDispatchServerConfig) => new EvidenceStore(config.workDir)
      },
      {
        provide: 'DELIVERYDISPATCH_LOCATION_STORE',
        inject: [DELIVERYDISPATCH_SAMPLE_CONFIG],
        useFactory: (config: DeliveryDispatchServerConfig) => createDeliveryDispatchLocationStore(config)
      }
    ]
  })(TrackingModule);

  return TrackingModule;
}

export { createTrackingModule };
