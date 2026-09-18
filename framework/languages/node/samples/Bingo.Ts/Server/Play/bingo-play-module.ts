import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { ZLinkUserSpotExecutionMode } from '@zlink-systems/framework';
import { bingoFrameworkProtobuf } from '../../Shared/Contracts/protobuf-framework-codec';
import { PlayerActorFactory } from './Infrastructure/ZLink/Actors/player-actor-factory';
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
    'nodeId',
    'peerNodeId',
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
            .messageFlow('normal');
          builder.addLocationStore(createBingoLocationStore(config));
          builder.addRelocationStore(createBingoRelocationStore(config));
          bingoLocationOptions(builder.configureLocations());
          builder.codecs().use(bingoFrameworkProtobuf);
          const mesh = builder.addRouteMesh(SampleNames.roomSpotNode)
            .setRoutingIdPrefix('play')
            .listen(config.playSpotEndpoint);
          const objectServer = mesh.objects().server();
          objectServer.addEntrySpot(BingoEntrySpot);
          // --8<-- [start:doc-execution-mode]
          // SpotWide is the default. Naming it here keeps the choice visible:
          // every callback of this room runs through one gate.
          objectServer.addSpotFactory(
            SampleNames.roomSpotType,
            BingoRoomSpot,
            (factory) => factory
              .executionMode(ZLinkUserSpotExecutionMode.SpotWide)
              .disableRelocation()
          );
          // --8<-- [end:doc-execution-mode]
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
      BingoEntrySpot,
      RoomRouterReadinessHandler
    ]
  })(BingoPlayModule);

  return BingoPlayModule;
}

export { createBingoPlayModule };
