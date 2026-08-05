import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { SampleNames } from '../Configuration/sample-names';
import { createSupportChatLocationStore, supportChatLocationOptions } from '../Configuration/location-store';
import { SUPPORT_CHAT_CONFIG, createSupportChatConfigurationModule } from '../Configuration/sample-config';
import type { SupportChatServerConfig } from '../Configuration/sample-config';

function createSupportChatApiModule() {
  class SupportChatApiModule {}
  const configuration = createSupportChatConfigurationModule([
    'apiChannelEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir'
  ]);

  zlinkModule(__dirname, {
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [SUPPORT_CHAT_CONFIG],
        useFactory: (config: SupportChatServerConfig) => {
          const builder = zlinkFramework();
          builder.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
            .traceLogFile(`${config.logDir}/flow-api.log`)
            .traceLabel('api');
          builder.addLocationStore(createSupportChatLocationStore(config));
          supportChatLocationOptions(builder.configureLocations());
          const mesh = builder.addRouteMesh(SampleNames.conversationSpotMesh)
            .listen(config.apiChannelEndpoint)
            .setRoutingIdPrefix('support-api');
          mesh.objects().client();
          builder.addClientServerChannel(SampleNames.apiChannel)
            .server()
            .listen()
            .addHandlerGroup('api');
          return builder.build();
        }
      })
    ],
    providers: []
  })(SupportChatApiModule);

  return SupportChatApiModule;
}

export { createSupportChatApiModule };
