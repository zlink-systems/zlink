import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { bingoFrameworkProtobuf } from '../../Shared/Contracts/protobuf-framework-codec';
import { BINGO_SAMPLE_CONFIG, createBingoConfigurationModule } from '../Configuration/sample-config';
import { SampleNames } from '../Configuration/sample-names';
import { bingoLocationOptions, createBingoLocationStore } from '../Configuration/location-store';
import { createBingoRelocationStore } from '../Configuration/relocation-store';
import { BingoMatchReservationStore } from './bingo-match-reservation-store';
import { BingoMatchmaker, BingoMatchmakerIdleTimer } from './bingo-matchmaker';
import type { BingoSampleConfig } from '../Configuration/sample-config';

function createBingoMatchmakingModule() {
  class BingoMatchmakingModule {}
  const configuration = createBingoConfigurationModule([
    'matchmakingEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir'
  ]);
  zlinkModule(__dirname, {
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [BINGO_SAMPLE_CONFIG],
        useFactory: (config: BingoSampleConfig) => {
          const builder = zlinkFramework();
          builder.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
            .traceLogFile(`${config.logDir}/flow-matchmaking.log`)
            .traceLabel('matchmaking');
          builder.addLocationStore(createBingoLocationStore(config));
          builder.addRelocationStore(createBingoRelocationStore(config));
          bingoLocationOptions(builder.configureLocations());
          builder.codecs().use(bingoFrameworkProtobuf);
          builder.addRouteMesh(SampleNames.matchmakingMeshName)
            .setRoutingIdPrefix('matchmaking')
            .listen(config.matchmakingEndpoint)
            .objects().server()
            .addInstanceSpotFactory(
              SampleNames.matchmakerSpotType,
              BingoMatchmaker,
              (factory) => factory.recreateOnRelocation()
            );
          return builder.build();
        }
      })
    ],
    providers: [
      BingoMatchReservationStore,
      BingoMatchmaker,
      BingoMatchmakerIdleTimer
    ]
  })(BingoMatchmakingModule);
  return BingoMatchmakingModule;
}

export { createBingoMatchmakingModule };
