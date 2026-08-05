import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { SampleNames } from '../Configuration/sample-settings';
import { PlayActorFactory } from './Infrastructure/ZLink/Actors/play-actor-factory';
import { PlayActorRelocationAdapter } from './Infrastructure/ZLink/Actors/play-actor-relocation-adapter';
import {
  PlayEntrySpot
} from './Infrastructure/ZLink/Spots/EntrySpot/play-entry-spot';
import {
  MilestoneObserverRegistry,
  PendingActorDestroyRegistry
} from './Infrastructure/ZLink/Spots/EntrySpot/entry-spot-registries';
import { TicTacToeGameSpot } from './Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot';
import { PlaySessionFactory } from './Infrastructure/ZLink/Sessions/play-session-factory';
import { PLAY_STREAM_ENDPOINT } from './play-tokens';
import { createTicTacToeLocationStore } from '../Configuration/location-store';
import { createTicTacToeRelocationStore } from '../Configuration/relocation-store';
import { TICTACTOE_SAMPLE_CONFIG, createTicTacToeConfigurationModule } from '../Configuration/sample-config';
import type { TicTacToeSampleConfig } from '../Configuration/sample-config';
function createTicTacToePlayModule() {
  class TicTacToePlayModule {}
  const configuration = createTicTacToeConfigurationModule([
    'apiEndpoints',
    'playSpotEndpoint',
    'playStreamEndpoint',
    'playEndpoints',
    'redisEndpoint',
    'redisKeyPrefix',
    'peerPlaySpotEndpoint',
    'instanceName',
    'logDir'
  ]);

  zlinkModule(__dirname, {
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [TICTACTOE_SAMPLE_CONFIG],
        useFactory: (config: TicTacToeSampleConfig) => {
          const builder = zlinkFramework();
          builder.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
            .traceLogFile(`${config.logDir}/flow-${config.instanceName}.log`)
            .traceLabel(config.instanceName);
          builder.addLocationStore(createTicTacToeLocationStore(config));
          builder.addRelocationStore(createTicTacToeRelocationStore(config));
          builder.addStreamNode(SampleNames.playStream)
            .enableActorDispatch()
            .bind(config.playStreamEndpoint)
            .registerSession(PlaySessionFactory);
          const mesh = builder.addRouteMesh(SampleNames.playSpotNode)
            .listen(config.playSpotEndpoint)
            .setRoutingIdPrefix('tictactoe-play');
          const objectServer = mesh.objects().server();
          objectServer.addEntrySpot(PlayEntrySpot);
          objectServer.addSpotFactory(
            SampleNames.gameSpotType,
            TicTacToeGameSpot,
            (factory) => factory.disableRelocation()
          );
          objectServer.addActorFactory(
            SampleNames.playerActorType,
            PlayActorFactory,
            (factory) => factory.preserveStateWith(PlayActorRelocationAdapter)
          );
          mesh.channel(SampleNames.apiChannel).client();
          mesh.channel(SampleNames.playerMilestoneChannel).server();
          for (const endpoint of config.apiEndpoints) {
            mesh.peerConnections().connect(endpoint);
          }
          mesh.peerConnections().connect(config.peerPlaySpotEndpoint);
          return builder.build();
        }
      })
    ],
    providers: [
      {
        provide: PLAY_STREAM_ENDPOINT,
        inject: [TICTACTOE_SAMPLE_CONFIG],
        useFactory: (config: TicTacToeSampleConfig) => config.playStreamEndpoint
      },
      PlayActorFactory,
      PlayActorRelocationAdapter,
      MilestoneObserverRegistry,
      PendingActorDestroyRegistry,
      PlayEntrySpot,
      PlaySessionFactory,
    ]
  })(TicTacToePlayModule);

  return TicTacToePlayModule;
}

export { createTicTacToePlayModule };
