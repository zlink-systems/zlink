import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { bingoFrameworkProtobuf } from '../../Shared/Contracts/protobuf-framework-codec';
import { SessionAuthenticator } from './Sessions/Handlers/authenticate-session-handler';
import { BingoSessionFactory } from './Sessions/bingo-session';
import { SampleNames } from '../Configuration/sample-names';
import { BINGO_SAMPLE_CONFIG, createBingoConfigurationModule } from '../Configuration/sample-config';
import type { BingoSampleConfig } from '../Configuration/sample-config';
import { bingoLocationOptions, createBingoLocationStore } from '../Configuration/location-store';
import { createBingoRelocationStore } from '../Configuration/relocation-store';
import { bingoMeterProvider } from '../runtime-support';
import { RoomRouterReadinessHandler } from '../Configuration/room-router-readiness-handler';
function createBingoSessionModule() {
  class BingoSessionModule {}
  const configuration = createBingoConfigurationModule([
    'sessionEndpoint',
    'sessionSpotEndpoint',
    'sessionSpotPubSubEndpoint',
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
        useFactory: (endpoints: BingoSampleConfig) => {
          const builder = zlinkFramework();
          builder.options({
            metrics: { meterProvider: bingoMeterProvider },
          });
          builder.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
            .traceLogFile(`${endpoints.logDir}/flow-session.log`)
            .traceLabel('session');
          builder.addLocationStore(createBingoLocationStore(endpoints));
          builder.addRelocationStore(createBingoRelocationStore(endpoints));
          bingoLocationOptions(builder.configureLocations());
          builder.codecs().use(bingoFrameworkProtobuf);
          const mesh = builder.addRouteMesh(SampleNames.roomSpotNode)
            .setRoutingIdPrefix('session')
            .listen(endpoints.sessionSpotEndpoint);
          mesh.objects().client();
          builder.addClientServerChannel(SampleNames.apiChannel).client();
          return builder.addStreamNode(SampleNames.sessionStream)
            .enableActorDispatch()
            .bind(endpoints.sessionEndpoint)
            .registerSession(BingoSessionFactory)
          .build();
        }
      })
    ],
    providers: [
      BingoSessionFactory,
      RoomRouterReadinessHandler
    ]
  })(BingoSessionModule);

  return BingoSessionModule;
}

function getSessionAuthenticator(app: { get(token: unknown, options?: { strict?: boolean }): unknown }): InstanceType<typeof SessionAuthenticator> {
  return app.get(SessionAuthenticator, { strict: false }) as InstanceType<typeof SessionAuthenticator>;
}

export { createBingoSessionModule, getSessionAuthenticator };
