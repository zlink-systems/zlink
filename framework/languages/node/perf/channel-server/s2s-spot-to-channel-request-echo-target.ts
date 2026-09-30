import { RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { runRole } from '../server-support/server-application';
import { ChannelEchoHandler } from './channel-echo-only-scenario';

// The Channel echo target of §10.5: an automatic RouteMesh node (run Docker Redis) that serves this cell's
// ChannelName with the typed request handler. The measured operation lives in the Spot process; this side echoes.
export async function runS2sSpotToChannelRequestEchoTarget(config: RoleConfig): Promise<void> {
  if (config.role !== 'channel' || config.source) throw new Error('The Channel role is the echo target of this scenario.');
  await runRole({
    config,
    providers: [ChannelEchoHandler],
    configureFramework: (builder) => {
      builder.addRouteMesh(config.meshName!).setRoutingIdPrefix('perf-channel').listen(config.transportEndpoints.mesh).setAdvertiseHost('127.0.0.1')
        .channel(config.channelName!).server().addRequestHandler('PerfEchoRequest', ChannelEchoHandler);
    }
  }, new Measurement(config, config.source));
}
