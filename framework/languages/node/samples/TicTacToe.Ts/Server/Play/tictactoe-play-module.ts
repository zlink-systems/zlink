import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { SampleNames } from '../Configuration/sample-settings';
import { PlayActorFactory } from './Infrastructure/ZLink/Actors/play-actor-factory';
import { PlayActorRelocationAdapter } from './Infrastructure/ZLink/Actors/play-actor-relocation-adapter';
import {
  DeliverPlayNotificationEntryHandler,
  DeliverPlayNotificationHandler
} from './Infrastructure/ZLink/Actors/play-actor';
import {
  PlayEntrySpot
} from './Infrastructure/ZLink/Spots/EntrySpot/play-entry-spot';
import { MilestoneObserverRegistry } from './Infrastructure/ZLink/Spots/EntrySpot/entry-spot-registries';
import { PlayActorJoinGameHandler } from './Infrastructure/ZLink/Spots/EntrySpot/Handlers/play-actor-join-game-handler';
import { PlayActorObserveMilestoneHandler } from './Infrastructure/ZLink/Spots/EntrySpot/Handlers/play-actor-observe-milestone-handler';
import { PlayerWinMilestoneEventHandler } from './Infrastructure/ZLink/Spots/EntrySpot/Handlers/player-win-milestone-event-handler';
import { TicTacToeGameSpot } from './Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot';
import {
  PlayActorCurrentGameStateHandler,
  PlayActorPlaceMarkHandler
} from './Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play-actor-place-mark-handler';
import { PlayActorLeaveGameHandler } from './Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play-actor-leave-game-handler';
import { TicTacToeGameTimerHandler } from './Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/tictactoe-game-timer-handler';
import { PlaySessionFactory } from './Infrastructure/ZLink/Sessions/play-session-factory';
import { AuthenticatePlaySessionHandler } from './Infrastructure/ZLink/Sessions/Handlers/authenticate-play-session-handler';
import { PLAY_STREAM_ENDPOINT } from './play-tokens';
import { createTicTacToeLocationStore } from '../Configuration/location-store';
import { createTicTacToeRelocationStore } from '../Configuration/relocation-store';
import { TICTACTOE_SAMPLE_CONFIG, createTicTacToeConfigurationModule } from '../Configuration/sample-config';
import type { TicTacToeSampleConfig } from '../Configuration/sample-config';
function createTicTacToePlayModule() {
  class TicTacToePlayModule {}
  const configuration = createTicTacToeConfigurationModule([
    'apiEndpoints',
    'playIndex',
    'playSpotEndpoint',
    'playStreamEndpoint',
    'playEndpoints',
    'redisEndpoint',
    'redisKeyPrefix',
    'peerPlaySpotEndpoint',
    'instanceName',
    'logDir'
  ]);

  zlinkModule({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [TICTACTOE_SAMPLE_CONFIG],
        useFactory: (config: TicTacToeSampleConfig) => {
          const builder = zlinkFramework();
          builder.disableImplicitHandlerAutoRegistration();
          builder.configureDispatch()
            .messageFlow('normal');
          builder.addLocationStore(createTicTacToeLocationStore(config));
          builder.addRelocationStore(createTicTacToeRelocationStore(config));
          builder.addStreamNode(SampleNames.playStream)
            .enableActorDispatch()
            .bind(config.playStreamEndpoint)
            .registerSession(PlaySessionFactory);
          const apiChannel = builder.addClientServerChannel(SampleNames.apiChannel).client();
          for (const endpoint of config.apiEndpoints) {
            // request: AuthenticatePlayerReq is sent to one explicitly connected Api server.
            apiChannel.connect(endpoint);
          }
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
          mesh.channel(SampleNames.playerMilestoneChannel).server();
          if (config.playIndex === 0) {
            // RouteMesh: Play-A is the single initiator for the Play-A to Play-B connection.
            mesh.peerConnections().connect(config.peerPlaySpotEndpoint);
          }
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
      AuthenticatePlaySessionHandler,
      DeliverPlayNotificationEntryHandler,
      DeliverPlayNotificationHandler,
      MilestoneObserverRegistry,
      PlayActorCurrentGameStateHandler,
      PlayActorJoinGameHandler,
      PlayActorLeaveGameHandler,
      PlayActorObserveMilestoneHandler,
      PlayActorPlaceMarkHandler,
      PlayEntrySpot,
      PlaySessionFactory,
      PlayerWinMilestoneEventHandler,
      TicTacToeGameTimerHandler,
    ]
  })(TicTacToePlayModule);

  return TicTacToePlayModule;
}

export { createTicTacToePlayModule };
