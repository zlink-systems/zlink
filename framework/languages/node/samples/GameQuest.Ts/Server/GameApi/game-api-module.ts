import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { SampleNames } from '../../Shared/Configuration/sample-names';
import { createGameQuestLocationStore, gameQuestLocationOptions } from '../Configuration/location-store';
import { createGameQuestRelocationStore } from '../Configuration/relocation-store';
import { GAMEQUEST_INSTANCE_ID, GAMEQUEST_LOCATION_STORE } from '../Configuration/tokens';
import { GAMEQUEST_SAMPLE_CONFIG, createGameQuestConfigurationModule } from '../Configuration/sample-config';
import { GameplayActionService } from './Application/gameplay-action-service';
import { GameplayEventPublisher } from './Infrastructure/ZLink/gameplay-event-publisher';
import { GameQuestEntrySpot } from './Infrastructure/ZLink/gamequest-entry-spot';
import { GameQuestPlayerActor } from './Infrastructure/ZLink/gamequest-player-actor';
import { GameQuestSessionFactory } from './game-api-session';
import {
  GameQuestSelfCheckStore,
  GameplayStateStore,
  QuestEventStore,
  QuestReadModelStore
} from '../Shared/Store/quest-progress-store';
import type { ZLinkActorContext, ZLinkActorFactory } from '@zlink-systems/framework';
import type { GameQuestServerConfig } from '../Configuration/sample-config';

class GameQuestPlayerActorFactory implements ZLinkActorFactory {
  async create(context: ZLinkActorContext): Promise<GameQuestPlayerActor> {
    return new GameQuestPlayerActor(context.actorId, context);
  }
}

function createGameApiModule(instanceId: 'api-a' | 'api-b') {
  class GameApiModule {}
  const streamEndpointKey = instanceId === 'api-a' ? 'apiAStreamEndpoint' : 'apiBStreamEndpoint';
  const actorSpotEndpointKey = instanceId === 'api-a' ? 'apiAActorSpotEndpoint' : 'apiBActorSpotEndpoint';
  const configuration = createGameQuestConfigurationModule([
    streamEndpointKey,
    actorSpotEndpointKey,
    'redisEndpoint',
    'redisKeyPrefix',
    'logDir',
    'workDir',
    instanceId === 'api-a' ? 'apiAHttpUrl' : 'apiBHttpUrl'
  ]);

  zlinkModule(__dirname, {
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [GAMEQUEST_SAMPLE_CONFIG],
        useFactory: (config: GameQuestServerConfig) => {
          const builder = zlinkFramework();
          builder.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
            .traceLogFile(`${config.logDir}/flow-${instanceId}.log`)
            .traceLabel(instanceId);
          builder.addLocationStore(createGameQuestLocationStore(config));
          builder.addRelocationStore(createGameQuestRelocationStore(config));
          gameQuestLocationOptions(builder.configureLocations());
          builder.addStreamNode(SampleNames.playerStreamNode)
            .enableActorDispatch()
            .bind(config[streamEndpointKey])
            .registerSession(GameQuestSessionFactory);
          const mesh = builder.addRouteMesh(SampleNames.playerQuestSpotMesh)
            .listen(config[actorSpotEndpointKey])
            .setRoutingIdPrefix('gamequest-api');
          const objectServer = mesh.objects().server();
          objectServer.addEntrySpot(GameQuestEntrySpot);
          objectServer.addActorFactory(
            SampleNames.playerActorType,
            GameQuestPlayerActorFactory,
            (factory) => factory.disableRelocation()
          );
          return builder.build();
        }
      })
    ],
    providers: [
      { provide: GAMEQUEST_INSTANCE_ID, useValue: instanceId },
      {
        provide: GameplayStateStore,
        inject: [GAMEQUEST_SAMPLE_CONFIG],
        useFactory: (config: GameQuestServerConfig) => new GameplayStateStore(config.workDir)
      },
      {
        provide: QuestEventStore,
        inject: [GAMEQUEST_SAMPLE_CONFIG],
        useFactory: (config: GameQuestServerConfig) => new QuestEventStore(config.workDir)
      },
      {
        provide: QuestReadModelStore,
        inject: [GAMEQUEST_SAMPLE_CONFIG],
        useFactory: (config: GameQuestServerConfig) => new QuestReadModelStore(config.workDir)
      },
      {
        provide: GameQuestSelfCheckStore,
        useFactory: (gameplay: GameplayStateStore, events: QuestEventStore, readModel: QuestReadModelStore) =>
          new GameQuestSelfCheckStore(gameplay, events, readModel),
        inject: [GameplayStateStore, QuestEventStore, QuestReadModelStore]
      },
      {
        provide: GAMEQUEST_LOCATION_STORE,
        inject: [GAMEQUEST_SAMPLE_CONFIG],
        useFactory: (config: GameQuestServerConfig) => createGameQuestLocationStore(config)
      },
      GameplayActionService,
      GameplayEventPublisher,
      GameQuestEntrySpot,
      GameQuestPlayerActorFactory,
      GameQuestSessionFactory
    ]
  })(GameApiModule);

  return GameApiModule;
}

export { createGameApiModule };
