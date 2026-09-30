import type { ZLinkClientServerRuntime, ZLinkRouteClient, ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { ZLINK_CLIENT_SERVER_RUNTIME, ZLINK_ROUTE_CLIENT, ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import { RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { port } from '../server-support/endpoints';
import { runRole } from '../server-support/server-application';
import { ChannelEchoHandler, ChannelEchoOnlyScenario } from './channel-echo-only-scenario';

// §11.2 role host: manual RouteMesh or ClientServer, no Store and no objects.
export async function runChannelEchoOnly(config: RoleConfig): Promise<void> {
  if (config.role !== 'channel') throw new Error('ChannelServer supports the channel-echo-only source and target.');
  const measurement = new Measurement(config, config.source);
  let scenario: ChannelEchoOnlyScenario | undefined;
  const listenerEndpoint = Object.values(config.transportEndpoints)[0];
  await runRole({
    config,
    providers: [ChannelEchoHandler],
    configureFramework: (builder) => {
      if (config.topology === 'routemesh') {
        const mesh = builder.addRouteMesh(config.meshName!).listen(listenerEndpoint).setAdvertiseHost('127.0.0.1');
        if (config.source) {
          mesh.channel(config.channelName!).client();
          mesh.peerConnections().connect(config.peerEndpoint!);
        } else mesh.channel(config.channelName!).server().addRequestHandler('PerfEchoRequest', ChannelEchoHandler);
      } else if (config.topology === 'clientserver') {
        const channel = builder.addClientServerChannel(config.channelName!);
        if (config.source) channel.client().connect(config.peerEndpoint!);
        else channel.server().listen(port(listenerEndpoint)).setBindHost('127.0.0.1').setAdvertiseHost('127.0.0.1').addRequestHandler('PerfEchoRequest', ChannelEchoHandler);
      } else throw new Error('Unsupported channel topology.');
    },
    workload: () => scenario?.run,
    prepare: async (app) => {
      if (!config.source) return;
      scenario = new ChannelEchoOnlyScenario(app.get<ZLinkRouteClient>(ZLINK_ROUTE_CLIENT, { strict: false }), measurement, config,
        app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }), app.get<ZLinkClientServerRuntime>(ZLINK_CLIENT_SERVER_RUNTIME, { strict: false }));
      await scenario.prepare();
    }
  }, measurement);
}
