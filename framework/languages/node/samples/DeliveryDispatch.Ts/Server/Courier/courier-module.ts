import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { SampleNames } from '../../Shared/Configuration/sample-names';
import { CourierActorDirectory, CourierActorFactory } from './courier-actor';
import { CourierEntrySpot } from './courier-entry-spot';
import { createDeliveryDispatchLocationStore, deliveryDispatchLocationOptions } from '../Configuration/location-store';
import {
  DELIVERYDISPATCH_SAMPLE_CONFIG,
  createDeliveryDispatchConfigurationModule
} from '../Configuration/sample-config';
import type { DeliveryDispatchServerConfig } from '../Configuration/sample-config';

type CourierOptions = {
  courierId: string;
};

function createCourierActorNodeModule(options: CourierOptions) {
  class CourierActorNodeModule {}
  const directory = new CourierActorDirectory();
  const nodeLabel = options.courierId === 'courier-a' ? 'courier-a' : 'courier-b';
  const spotEndpointKey = options.courierId === 'courier-a'
    ? 'courierActorNode1SpotEndpoint'
    : 'courierActorNode2SpotEndpoint';
  const configuration = createDeliveryDispatchConfigurationModule([
    spotEndpointKey,
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
          const spotEndpoint = config[spotEndpointKey];
          const builder = zlinkFramework();
          builder.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
            .traceLogFile(`${config.logDir}/flow-${nodeLabel}.log`)
            .traceLabel(nodeLabel);
          builder.addLocationStore(createDeliveryDispatchLocationStore(config));
          deliveryDispatchLocationOptions(builder.configureLocations());
          const mesh = builder.addRouteMesh(SampleNames.courierMeshName)
              .listen(spotEndpoint).setRoutingIdPrefix('delivery-courier');
          const objectServer = mesh.objects().server();
          objectServer.addEntrySpot(CourierEntrySpot);
          objectServer.addActorFactory(
            SampleNames.courierActorType,
            CourierActorFactory,
            (factory) => factory.disableRelocation()
          );
          builder.addClientServerChannel(SampleNames.dispatchChannel).client();
          return builder.build();
        }
      })
    ],
    providers: [
      { provide: CourierActorDirectory, useValue: directory },
      CourierActorFactory,
      CourierEntrySpot
    ]
  })(CourierActorNodeModule);

  return CourierActorNodeModule;
}

export {
  createCourierActorNodeModule
};

export type {
  CourierOptions
};
