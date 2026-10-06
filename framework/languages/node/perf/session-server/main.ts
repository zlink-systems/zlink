import { Measurement } from '../shared/measurement';
import {
  PerfActorRelaySessionFactory,
  SessionActorSetup
} from '../server-support/actor-echo-support';
import { ObjectsReadiness, readConfig, runRole } from '../server-support/server-application';
import { PerfSessionFactory, SessionEchoHandler } from './perf-session';

const { config } = readConfig(process.argv.slice(2));
if (
  config.role !== 'session' ||
  config.source ||
  (config.scenario !== 'session-echo-only' && config.scenario !== 'cs-remote-session-actor-echo')
) {
  throw new Error('SessionServer supports the session receiver roles of §10.2 and §11.1.');
}
const measurement = new Measurement(config, config.source);
const remote = config.scenario === 'cs-remote-session-actor-echo';
const objects = remote
  ? new ObjectsReadiness(false, 'The expected Actors are not all bound to their Sessions yet.')
  : undefined;
runRole(
  {
    config,
    ...(objects ? { objects } : {}),
    providers: remote
      ? [SessionActorSetup, PerfActorRelaySessionFactory]
      : [PerfSessionFactory, SessionEchoHandler],
    configureFramework: (builder) => {
      if (!remote) {
        builder
          .addStreamNode('perf-session')
          .bind(config.transportEndpoints.stream)
          .registerSession(PerfSessionFactory);
        return;
      }
      // §10.2: an Object Client node; the Actors live in the separate Actor process.
      builder
        .addRouteMesh(config.meshName!)
        .listen(config.transportEndpoints.mesh)
        .setAdvertiseHost('127.0.0.1')
        .objects()
        .client();
      builder
        .addStreamNode('perf-session')
        .enableActorDispatch()
        .bind(config.transportEndpoints.stream)
        .registerSession(PerfActorRelaySessionFactory);
    }
  },
  measurement
).catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
