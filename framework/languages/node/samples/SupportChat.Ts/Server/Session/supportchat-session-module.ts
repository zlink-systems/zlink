import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { SampleNames } from '../Configuration/sample-names';
import { createSupportChatLocationStore, supportChatLocationOptions } from '../Configuration/location-store';
import {
  SupportChatSessionFactory,
  SupportChatSessionRouter
} from './Sessions/supportchat-session';
import { SUPPORT_CHAT_CONFIG, createSupportChatConfigurationModule } from '../Configuration/sample-config';
import type { SupportChatServerConfig } from '../Configuration/sample-config';

function createSupportChatSessionModule() {
  class SupportChatSessionModule {}
  const configuration = createSupportChatConfigurationModule([
    'sessionSpotEndpoint', 'sessionStreamEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir'
  ]);

  zlinkModule(__dirname, {
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [SUPPORT_CHAT_CONFIG],
        useFactory: (config: SupportChatServerConfig) => {
          const locationStore = createSupportChatLocationStore(config);
          const builder = zlinkFramework();
          builder.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
            .traceLogFile(`${config.logDir}/flow-session.log`)
            .traceLabel('session');
          builder.addLocationStore(locationStore);
          supportChatLocationOptions(builder.configureLocations());
          const mesh = builder.addRouteMesh(SampleNames.conversationSpotMesh)
            .listen(config.sessionSpotEndpoint).setRoutingIdPrefix('support-session');
          mesh.objects().client();
          builder.addClientServerChannel(SampleNames.apiChannel).client();
          return builder.addStreamNode(SampleNames.sessionStreamNode)
              .enableActorDispatch()
              .bind(config.sessionStreamEndpoint)
              .registerSession(SupportChatSessionFactory as never)
            .build();
        }
      })
    ],
    providers: [
      {
        provide: 'SUPPORTCHAT_LOCATION_STORE',
        inject: [SUPPORT_CHAT_CONFIG],
        useFactory: (config: SupportChatServerConfig) => createSupportChatLocationStore(config)
      },
      SupportChatSessionRouter,
      SupportChatSessionFactory
    ]
  })(SupportChatSessionModule);

  return SupportChatSessionModule;
}

export { createSupportChatSessionModule };
