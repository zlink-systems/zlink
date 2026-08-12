import { ZLinkModule, zlinkFramework, zlinkModule } from '@zlink-systems/nestjs';
import { CreateGameEndpoint } from './Handlers/create-game-http-handler';
import { AuthenticatePlayerHandler } from './Handlers/authenticate-player-handler';
import { PacketNames } from '../../Shared/Contracts/messages';
import { SampleNames } from '../Configuration/sample-settings';
import { TICTACTOE_SAMPLE_CONFIG, createTicTacToeConfigurationModule } from '../Configuration/sample-config';
import type { TicTacToeSampleConfig } from '../Configuration/sample-config';
import { createTicTacToeLocationStore } from '../Configuration/location-store';
function createTicTacToeApiModule() {
  class TicTacToeApiModule {}
  const configuration = createTicTacToeConfigurationModule([
    'apiHttpEndpoint', 'apiEndpoints', 'apiSpotEndpoint', 'apiIndex', 'playSpotEndpoints', 'playEndpoints',
    'redisEndpoint', 'redisKeyPrefix', 'logDir'
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
          const apiEndpoint = new URL(config.apiEndpoints[config.apiIndex]);
          // request: AuthenticatePlayerReq returns AuthenticatePlayerRes.
          builder.addClientServerChannel(SampleNames.apiChannel).server()
            .setBindHost(apiEndpoint.hostname)
            .listen(Number(apiEndpoint.port))
            .addRequestHandler(PacketNames.authenticatePlayerReq, AuthenticatePlayerHandler);
          const mesh = builder.addRouteMesh(SampleNames.playSpotNode)
            .listen(config.apiSpotEndpoint)
            .setRoutingIdPrefix('tictactoe-api');
          mesh.objects().client();
          // RouteMesh: each Api explicitly connects to both Play object servers.
          for (const endpoint of config.playSpotEndpoints) {
            mesh.peerConnections().connect(endpoint);
          }
          return builder.build();
        }
      })
    ],
    providers: [
      AuthenticatePlayerHandler,
      CreateGameEndpoint
    ]
  })(TicTacToeApiModule);

  return TicTacToeApiModule;
}

function getCreateGameEndpoint(app: { get(token: unknown, options?: { strict?: boolean }): unknown }): InstanceType<typeof CreateGameEndpoint> {
  return app.get(CreateGameEndpoint, { strict: false }) as InstanceType<typeof CreateGameEndpoint>;
}

export { createTicTacToeApiModule, getCreateGameEndpoint };
