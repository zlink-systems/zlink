import { ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import type { ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { Measurement } from '../shared/measurement';
import {
  ActorEchoRequestHandler,
  ActorEchoSendHandler,
  PERF_ACTOR_TYPE,
  PerfActorFactory,
  PerfEntrySpot,
  watchActorPlacement
} from '../server-support/actor-echo-support';
import { ObjectsReadiness, readConfig, runRole } from '../server-support/server-application';

const { config } = readConfig(process.argv.slice(2));
if (
  config.role !== 'actor' ||
  ![
    'cs-remote-session-actor-echo',
    'actor-no-bind-request-echo',
    'actor-no-bind-send-send-echo'
  ].includes(config.scenario)
) {
  throw new Error('ActorServer runs the actor role of §10.2, §10.9 and §10.10.');
}
const measurement = new Measurement(config, config.source);
const readiness = new ObjectsReadiness(false, 'No typed probe has reached an Actor yet.');
// The Actor Object Server hosts the Actors that a remote Session binds or an ActorCaller addresses by ActorId.
runRole(
  {
    config,
    objects: readiness,
    providers: [
      PerfActorFactory,
      PerfEntrySpot,
      config.mode === 'send-send' ? ActorEchoSendHandler : ActorEchoRequestHandler
    ],
    configureFramework: (builder) => {
      const mesh = builder
        .addRouteMesh(config.meshName!)
        .listen(config.transportEndpoints.mesh)
        .setAdvertiseHost('127.0.0.1');
      // §10.10: the Actor answers through the public Channel client; the caller is the return channel Server.
      if (config.mode === 'send-send') mesh.channel(config.channelName!).client();
      mesh
        .objects()
        .server()
        .addEntrySpot(PerfEntrySpot)
        .addActorFactory(PERF_ACTOR_TYPE, PerfActorFactory, (factory) =>
          factory.disableRelocation()
        );
    },
    prepare: async (app) => {
      watchActorPlacement(
        config,
        measurement,
        readiness,
        app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false })
      );
    }
  },
  measurement
).catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
