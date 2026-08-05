import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { CreateGameEndpoint } from './Handlers/create-game-http-handler';
import { SampleNames } from '../Configuration/sample-settings';
import { TICTACTOE_SAMPLE_CONFIG, createTicTacToeConfigurationModule } from '../Configuration/sample-config';
import type { TicTacToeSampleConfig } from '../Configuration/sample-config';
import { createTicTacToeLocationStore } from '../Configuration/location-store';
function createTicTacToeApiModule() {
  class TicTacToeApiModule {}
  const configuration = createTicTacToeConfigurationModule([
    'apiHttpEndpoint', 'apiEndpoints', 'apiIndex', 'playSpotEndpoints', 'playEndpoints',
    'redisEndpoint', 'redisKeyPrefix', 'logDir'
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
            .traceLogFile(`${config.logDir}/flow-api-${config.apiIndex}.log`)
            .traceLabel(`api-${config.apiIndex}`);
          builder.addLocationStore(createTicTacToeLocationStore(config));
          const mesh = builder.addRouteMesh(SampleNames.playSpotNode)
            .listen(config.apiEndpoints[config.apiIndex])
            .setRoutingIdPrefix('tictactoe-api');
          mesh.channel(SampleNames.apiChannel).server()
            .addHandlerGroup('api');
          mesh.objects().client();
          for (const endpoint of config.playSpotEndpoints) {
            mesh.peerConnections().connect(endpoint);
          }
          return builder.build();
        }
      })
    ],
    providers: [
      CreateGameEndpoint
    ]
  })(TicTacToeApiModule);

  return TicTacToeApiModule;
}

function getCreateGameEndpoint(app: { get(token: unknown, options?: { strict?: boolean }): unknown }): InstanceType<typeof CreateGameEndpoint> {
  return app.get(CreateGameEndpoint, { strict: false }) as InstanceType<typeof CreateGameEndpoint>;
}

export { createTicTacToeApiModule, getCreateGameEndpoint };
