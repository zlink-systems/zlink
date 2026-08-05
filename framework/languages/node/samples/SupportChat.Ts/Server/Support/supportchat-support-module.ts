import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { SampleNames } from '../Configuration/sample-names';
import { createSupportChatLocationStore, supportChatLocationOptions } from '../Configuration/location-store';
import { AgentAssignmentService } from './Application/ConversationAssignment/agent-assignment-service';
import { AgentAvailabilityDirectory } from './Application/ConversationAssignment/agent-availability-directory';
import { SupportConversationAllocator } from './Application/ConversationAssignment/support-conversation-allocator';
import { SupportActorDirectory } from './Infrastructure/ZLink/Actors/support-actor-directory';
import { SupportUserActorFactory } from './Infrastructure/ZLink/Actors/support-user-actor-factory';
import { ConversationSpot } from './Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot';
import { SupportNotificationPublisher } from './Infrastructure/ZLink/Spots/ConversationSpot/Notifications/support-notification-publisher';
import { SupportEntrySpot } from './Infrastructure/ZLink/Spots/EntrySpot/support-entry-spot';
import { SUPPORT_CHAT_CONFIG, createSupportChatConfigurationModule } from '../Configuration/sample-config';
import type { SupportChatServerConfig } from '../Configuration/sample-config';

function createSupportChatSupportModule() {
  class SupportChatSupportModule {}
  const configuration = createSupportChatConfigurationModule([
    'supportSpotEndpoint',
    'redisEndpoint',
    'redisKeyPrefix',
    'logDir'
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
            .traceLogFile(`${config.logDir}/flow-support.log`)
            .traceLabel('support');
          builder.addLocationStore(createSupportChatLocationStore(config));
          supportChatLocationOptions(builder.configureLocations());
          const mesh = builder.addRouteMesh(SampleNames.conversationSpotMesh)
              .listen(config.supportSpotEndpoint).setRoutingIdPrefix('support-owner');
          const objectServer = mesh.objects().server();
          objectServer.addEntrySpot(SupportEntrySpot);
          objectServer.addSpotFactory(
            SampleNames.conversationSpotType,
            ConversationSpot,
            (factory) => factory.disableRelocation()
          );
          objectServer.addActorFactory(
            SampleNames.supportActorType,
            SupportUserActorFactory,
            (factory) => factory.disableRelocation()
          );
          mesh.channel(SampleNames.conversationSpotMesh).server();
          builder.addClientServerChannel(SampleNames.apiChannel).client();
          return builder.build();
        }
      })
    ],
    providers: [
      AgentAvailabilityDirectory,
      AgentAssignmentService,
      SupportConversationAllocator,
      SupportActorDirectory,
      SupportUserActorFactory,
      ConversationSpot,
      SupportEntrySpot,
      SupportNotificationPublisher
    ]
  })(SupportChatSupportModule);

  return SupportChatSupportModule;
}

export { createSupportChatSupportModule };
