import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { bingoFrameworkProtobuf } from '../../Shared/Contracts/protobuf-framework-codec';
import { PlayerActorFactory } from './Infrastructure/ZLink/Actors/player-actor-factory';
import { PendingBingoActorDestroyRegistry } from './Infrastructure/ZLink/Actors/player-actor-lifecycle-handlers';
import { PlayerActorRelocationAdapter } from './Infrastructure/ZLink/Actors/player-actor-relocation-adapter';
import { BingoEntrySpot } from './Infrastructure/ZLink/Spots/EntrySpot/bingo-entry-spot';
import { BingoRoomSpot } from './Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot';
import { SampleNames } from '../Configuration/sample-names';
import { BINGO_SAMPLE_CONFIG, createBingoConfigurationModule } from '../Configuration/sample-config';
import type { BingoSampleConfig } from '../Configuration/sample-config';
import { bingoLocationOptions, createBingoLocationStore } from '../Configuration/location-store';
import { createBingoRelocationStore } from '../Configuration/relocation-store';
import { bingoMeterProvider } from '../runtime-support';
import { RoomRouterReadinessHandler } from '../Configuration/room-router-readiness-handler';
function createBingoPlayModule() {
  class BingoPlayModule {}
  const configuration = createBingoConfigurationModule([
    'playSpotEndpoint',
    'playSpotPubSubEndpoint',
    'redisEndpoint',
    'redisKeyPrefix',
    'logDir'
  ]);

  zlinkModule(__dirname, {
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [BINGO_SAMPLE_CONFIG],
        useFactory: (config: BingoSampleConfig) => {
          const builder = zlinkFramework();
          builder.options({
            metrics: { meterProvider: bingoMeterProvider },
          });
          builder.setApplicationVersion(1n);
          builder.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
            .traceLogFile(`${config.logDir}/flow-play.log`)
            .traceLabel('play');
          builder.addLocationStore(createBingoLocationStore(config));
          builder.addRelocationStore(createBingoRelocationStore(config));
          bingoLocationOptions(builder.configureLocations());
          builder.codecs().use(bingoFrameworkProtobuf);
          const mesh = builder.addRouteMesh(SampleNames.roomSpotNode)
            .setRoutingIdPrefix('play')
            .listen(config.playSpotEndpoint);
          const objectServer = mesh.objects().server();
          objectServer.addEntrySpot(BingoEntrySpot);
          objectServer.addSpotFactory(
            SampleNames.roomSpotType,
            BingoRoomSpot,
            (factory) => factory.disableRelocation()
          );
          objectServer.addActorFactory(
            SampleNames.playerActorType,
            PlayerActorFactory,
            (factory) => factory.preserveStateWith(PlayerActorRelocationAdapter)
          );
          builder.addClientServerChannel(SampleNames.apiChannel).client();
          mesh.channel(SampleNames.roomRouteChannel).server();
          mesh.channel(SampleNames.roomRewardChannel).server();
          return builder.build();
        }
      })
    ],
    providers: [
      PlayerActorFactory,
      PlayerActorRelocationAdapter,
      PendingBingoActorDestroyRegistry,
      BingoEntrySpot,
      RoomRouterReadinessHandler
    ]
  })(BingoPlayModule);

  return BingoPlayModule;
}

export { createBingoPlayModule };
