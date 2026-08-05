import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { bingoFrameworkProtobuf } from '../../Shared/Contracts/protobuf-framework-codec';
import { SampleNames } from '../Configuration/sample-names';
import { BINGO_SAMPLE_CONFIG, createBingoConfigurationModule } from '../Configuration/sample-config';
import type { BingoSampleConfig } from '../Configuration/sample-config';
import { bingoLocationOptions, createBingoLocationStore } from '../Configuration/location-store';
import { bingoMeterProvider } from '../runtime-support';
import { RoomRouterReadinessHandler } from '../Configuration/room-router-readiness-handler';
import { BingoPlayerRecordStore } from './Handlers/player-record-handlers';
function createBingoApiModule() {
  class BingoApiModule {}
  const configuration = createBingoConfigurationModule([
    'apiEndpoint',
    'apiMatchmakingEndpoint',
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
          builder.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
            .traceLogFile(`${config.logDir}/flow-api.log`)
            .traceLabel('api');
          builder.addLocationStore(createBingoLocationStore(config));
          bingoLocationOptions(builder.configureLocations());
          builder.codecs().use(bingoFrameworkProtobuf);
          const apiMesh = builder.addRouteMesh(SampleNames.playMeshName)
            .setRoutingIdPrefix('api')
            .listen(config.apiEndpoint);
          apiMesh.objects().client();
          builder.addClientServerChannel(SampleNames.apiChannel)
            .server()
            .listen()
            .addHandlerGroup('api');
          builder.addRouteMesh(SampleNames.matchmakingMeshName)
            .setRoutingIdPrefix('api-matchmaking')
            .listen(config.apiMatchmakingEndpoint)
            .objects().client();
          return builder.build();
        }
      })
    ],
    providers: [BingoPlayerRecordStore, RoomRouterReadinessHandler]
  })(BingoApiModule);

  return BingoApiModule;
}

export { createBingoApiModule };
