import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { createShoppingMallLocationStore, shoppingMallLocationOptions } from '../Configuration/location-store';
import { createShoppingMallRelocationStore } from '../Configuration/relocation-store';
import { SHOPPINGMALL_SAMPLE_CONFIG, createShoppingMallConfigurationModule } from '../Configuration/sample-config';
import type { ShoppingMallServerConfig } from '../Configuration/sample-config';
import { OrderStore } from '../Shared/Store/order-store';
import { SampleNames } from '../../Shared/Configuration/sample-names';
import { OrderWorkflowService } from './Application/OrderWorkflow/order-workflow-service';
import { OrderWorkflowSpot } from './Infrastructure/ZLink/Spots/OrderWorkflowSpot/order-workflow-spot';
import { SHOPPINGMALL_ROLE } from './order-workflow-tokens';

function createShoppingMallWorkflowModule(role: string): Function {
  class ShoppingMallWorkflowModule {}
  const configuration = createShoppingMallConfigurationModule([
    role === SampleNames.workflowA ? 'workflowAHttpUrl' : 'workflowBHttpUrl',
    role === SampleNames.workflowA ? 'workflowASpotEndpoint' : 'workflowBSpotEndpoint',
    role === SampleNames.workflowA ? 'workflowASpotPubEndpoint' : 'workflowBSpotPubEndpoint',
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
        inject: [SHOPPINGMALL_SAMPLE_CONFIG],
        useFactory: (config: ShoppingMallServerConfig) => {
          const builder = zlinkFramework();
          builder.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
            .traceLogFile(`${config.logDir}/flow-${role}.log`)
            .traceLabel(role);
          builder.addLocationStore(createShoppingMallLocationStore(config));
          builder.addRelocationStore(createShoppingMallRelocationStore(config));
          shoppingMallLocationOptions(builder.configureLocations());
          const mesh = builder.addRouteMesh(SampleNames.orderWorkflowSpotMesh)
            .listen(workflowSpotEndpointForRole(role, config));
          mesh.objects().server().addInstanceSpotFactory(
            SampleNames.orderWorkflowSpotType,
            OrderWorkflowSpot,
            (factory) => factory.recreateOnRelocation()
          );
          return builder.build();
        }
      })
    ],
    providers: [
      { provide: SHOPPINGMALL_ROLE, useValue: role },
      {
        provide: OrderStore,
        inject: [SHOPPINGMALL_SAMPLE_CONFIG],
        useFactory: (config: ShoppingMallServerConfig) => new OrderStore(config.workDir)
      },
      OrderWorkflowService
    ]
  })(ShoppingMallWorkflowModule);
  return ShoppingMallWorkflowModule;
}

function workflowSpotEndpointForRole(role: string, values: ShoppingMallServerConfig): string {
  if (role === SampleNames.workflowA) {
    return values.workflowASpotEndpoint;
  }
  if (role === SampleNames.workflowB) {
    return values.workflowBSpotEndpoint;
  }
  throw new Error(`Role '${role}' is not an OrderWorkflow role.`);
}

export { createShoppingMallWorkflowModule };
