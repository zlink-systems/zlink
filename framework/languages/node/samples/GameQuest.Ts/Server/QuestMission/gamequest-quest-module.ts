import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { createGameQuestLocationStore, gameQuestLocationOptions } from '../Configuration/location-store';
import { createGameQuestRelocationStore } from '../Configuration/relocation-store';
import { GAMEQUEST_INSTANCE_ID, GAMEQUEST_LOCATION_STORE } from '../Configuration/tokens';
import { GAMEQUEST_SAMPLE_CONFIG, createGameQuestConfigurationModule } from '../Configuration/sample-config';
import {
  GameplayStateStore,
  QuestEventStore,
  QuestReadModelStore
} from '../Shared/Store/quest-progress-store';
import { SampleNames } from '../../Shared/Configuration/sample-names';
import { QuestEventProcessor } from './Application/quest-event-processor';
import { PlayerQuestNotifier } from './Infrastructure/ZLink/player-quest-notifier';
import { PlayerQuestSpotProvisioner } from './Infrastructure/ZLink/player-quest-spot-provisioner';
import { PlayerQuestSpot } from './Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot';
import type { GameQuestServerConfig } from '../Configuration/sample-config';

function createQuestMissionModule(instanceId: 'mission-a' | 'mission-b') {
  class GameQuestQuestModule {}
  const spotRouterEndpointKey = instanceId === 'mission-a'
    ? 'missionASpotRouterEndpoint'
    : 'missionBSpotRouterEndpoint';
  const configuration = createGameQuestConfigurationModule([
    spotRouterEndpointKey,
    instanceId === 'mission-a' ? 'missionAHttpUrl' : 'missionBHttpUrl',
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
          const spotMesh = builder.addRouteMesh(SampleNames.playerQuestSpotMesh)
            .listen(config[spotRouterEndpointKey])
            .setRoutingIdPrefix('gamequest-mission');
          spotMesh.objects().server().addInstanceSpotFactory(
            SampleNames.playerQuestSpotType,
            PlayerQuestSpot,
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
        provide: GAMEQUEST_LOCATION_STORE,
        inject: [GAMEQUEST_SAMPLE_CONFIG],
        useFactory: (config: GameQuestServerConfig) => createGameQuestLocationStore(config)
      },
      QuestEventProcessor,
      PlayerQuestNotifier,
      PlayerQuestSpotProvisioner
    ]
  })(GameQuestQuestModule);

  return GameQuestQuestModule;
}

export { createQuestMissionModule };
