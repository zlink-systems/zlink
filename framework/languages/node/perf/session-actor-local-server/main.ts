import { Measurement } from '../shared/measurement';
import { ActorEchoRequestHandler, PERF_ACTOR_TYPE, PerfActorFactory, PerfActorRelaySessionFactory, PerfEntrySpot, SessionActorSetup } from '../server-support/actor-echo-support';
import { ObjectsReadiness, readConfig, runRole } from '../server-support/server-application';

const { config } = readConfig(process.argv.slice(2));
if (config.scenario !== 'cs-local-session-actor-echo' || config.role !== 'session-actor-local') {
  throw new Error('SessionActorLocalServer runs the cs-local-session-actor-echo role.');
}
const measurement = new Measurement(config, config.source);
const readiness = new ObjectsReadiness(false, 'No Actor is bound to a session yet.');
// §10.1: the STREAM session and the Actors it binds live on this one Object Server node.
runRole({
  config,
  objects: readiness,
  providers: [{ provide: ObjectsReadiness, useValue: readiness }, SessionActorSetup, PerfActorRelaySessionFactory, PerfActorFactory, PerfEntrySpot, ActorEchoRequestHandler],
  configureFramework: (builder) => {
    builder.addRouteMesh(config.meshName!).listen(config.transportEndpoints.mesh).setAdvertiseHost('127.0.0.1')
      .objects().server().addEntrySpot(PerfEntrySpot).addActorFactory(PERF_ACTOR_TYPE, PerfActorFactory, (factory) => factory.disableRelocation());
    builder.addStreamNode('perf-session').enableActorDispatch().bind(config.transportEndpoints.stream).registerSession(PerfActorRelaySessionFactory);
  }
}, measurement).catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
