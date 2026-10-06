package systems.zlink.framework.perf;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateCall;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorGetOrCreateCall;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkPlacementSnapshot;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionActors;
import systems.zlink.framework.streams.ZLinkSessionClient;
import systems.zlink.framework.streams.ZLinkSessionContext;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeUnit;

class ObjectsReadinessTest {
    private static RoleConfig config(String role, List<String> actorIds) {
        return new RoleConfig(
                "test",
                "cs-remote-session-actor-echo/1024/test",
                "a".repeat(64),
                "java",
                role,
                0,
                "cs-remote-session-actor-echo",
                "request",
                "ordinary",
                "routemesh",
                null,
                "mesh",
                Map.of(),
                null,
                "",
                "",
                false,
                "ObjectServer",
                true,
                null,
                List.of(),
                actorIds,
                null,
                null,
                null,
                "Immediate",
                new RoleConfig.Workload(
                        1024, .01, .01, null, actorIds.size(), 1, null, 100, 100, 5000, 100),
                null,
                Map.of());
    }

    @Test
    void sessionReadinessRequiresEveryExpectedActorToBeBound() {
        RoleConfig config = config("session", List.of("actor-0", "actor-1"));
        Measurement measurement = new Measurement(config, false);
        ObjectsReadiness readiness = new ObjectsReadiness(false, "Actors are not yet bound.");
        SessionActorSetup setup =
                new SessionActorSetup(config, measurement, readiness, new ActorManager());
        ZLinkSessionActors actors = new SessionActors();
        ZLinkSessionContext session = new SessionContext(actors);

        setup.prepare(session, ZLinkMessage.of(measurement.request(0, 1, true)))
                .toCompletableFuture()
                .join();
        assertFalse(readiness.ready());
        setup.prepare(session, ZLinkMessage.of(measurement.request(1, 1, true)))
                .toCompletableFuture()
                .join();

        assertTrue(readiness.ready());
    }

    @Test
    void actorReadinessRequiresEveryExpectedActorToBeActive() throws Exception {
        RoleConfig config = config("actor", List.of("actor-0", "actor-1"));
        Measurement measurement = new Measurement(config, false);
        ObjectsReadiness readiness = new ObjectsReadiness(false, "Actors are not active yet.");
        CountedMesh mesh = new CountedMesh();
        ActorPlacementWatcher watcher =
                new ActorPlacementWatcher(config, measurement, readiness, mesh);
        watcher.start();
        try {
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
            while (readiness.evidence().isEmpty() && System.nanoTime() < deadline) {
                Thread.onSpinWait();
            }
            assertFalse(readiness.evidence().isEmpty(), "the placement snapshot was not observed");
            assertFalse(readiness.ready());
        } finally {
            measurement.start(
                    new PerfTriggerRequest("test", config.cellId(), "0", "warmup"),
                    () -> CompletableFuture.completedFuture(null));
            measurement.phaseTask().get(2, TimeUnit.SECONDS);
        }
    }

    private static final class ActorManager implements ZLinkActorManager {
        @Override
        public ZLinkActorCreateCall create(String actorId, String actorType) {
            throw new UnsupportedOperationException();
        }

        @Override
        public CompletableFuture<Optional<ActorRef>> find(String actorId) {
            return CompletableFuture.completedFuture(Optional.empty());
        }

        @Override
        public CompletableFuture<Optional<systems.zlink.framework.spots.SpotRef>> findSpot(
                String actorId) {
            return CompletableFuture.completedFuture(Optional.empty());
        }

        @Override
        public CompletableFuture<Boolean> destroy(ActorRef actor) {
            return CompletableFuture.completedFuture(false);
        }

        @Override
        public ZLinkActorGetOrCreateCall getOrCreate(String actorId, String actorType) {
            return new GetOrCreate(actorId);
        }
    }

    private static final class GetOrCreate implements ZLinkActorGetOrCreateCall {
        private final String actorId;

        GetOrCreate(String actorId) {
            this.actorId = actorId;
        }

        @Override
        public ZLinkActorGetOrCreateCall inMesh(String meshName) {
            return this;
        }

        @Override
        public ZLinkActorGetOrCreateCall request(Object request) {
            return this;
        }

        @Override
        public ZLinkActorGetOrCreateCall request(ZLinkMessage request) {
            return this;
        }

        @Override
        public ZLinkActorGetOrCreateCall timeout(Duration timeout) {
            return this;
        }

        @Override
        public CompletableFuture<ZLinkActorCreateResult> submit() {
            ActorRef actor = new ActorRef(actorId, 1, "mesh", RoutingId.from(1));
            return CompletableFuture.completedFuture(
                    new ZLinkActorCreateResult.Created(actor, null));
        }

        @Override
        public CompletableFuture<ZLinkActorCreateResult> yield() {
            return submit();
        }
    }

    private static final class SessionActors implements ZLinkSessionActors {
        @Override
        public List<ZLinkSessionActor> bound() {
            return List.of();
        }

        @Override
        public CompletableFuture<ZLinkSessionActor> bind(
                systems.zlink.framework.actors.ZLinkActor actor) {
            throw new UnsupportedOperationException();
        }

        @Override
        public CompletableFuture<ZLinkSessionActor> bind(ActorRef actor) {
            throw new UnsupportedOperationException();
        }

        @Override
        public CompletableFuture<ZLinkSessionActor> bindOrGet(ActorRef actor) {
            return CompletableFuture.completedFuture(new BoundActor(actor));
        }

        @Override
        public Optional<ZLinkSessionActor> find(String actorId) {
            return Optional.empty();
        }
    }

    private record BoundActor(ActorRef ref) implements ZLinkSessionActor {
        @Override
        public String actorId() {
            return ref.actorId();
        }

        @Override
        public systems.zlink.framework.actors.ActorRef ref() {
            return ref;
        }

        @Override
        public CompletableFuture<Void> relay(ZLinkMessage payload) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletableFuture<Void> notifyDisconnected() {
            return CompletableFuture.completedFuture(null);
        }
    }

    private record SessionContext(ZLinkSessionActors actors) implements ZLinkSessionContext {
        @Override
        public String sessionId() {
            return "session";
        }

        @Override
        public Optional<RoutingId> routingId() {
            return Optional.empty();
        }

        @Override
        public Optional<String> localAddr() {
            return Optional.empty();
        }

        @Override
        public Optional<String> remoteAddr() {
            return Optional.empty();
        }

        @Override
        public ZLinkSessionClient client() {
            return null;
        }

        @Override
        public CompletableFuture<Void> close() {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class CountedMesh implements ZLinkRouteMeshRuntime {
        @Override
        public ZLinkMeshNodeSnapshot snapshot(String meshName) {
            return new ZLinkMeshNodeSnapshot(
                    meshName,
                    null,
                    true,
                    1,
                    List.of(),
                    List.of(),
                    new ZLinkPlacementSnapshot(true, 1, 0, Optional.empty()),
                    1,
                    Instant.EPOCH);
        }

        @Override
        public Flow.Publisher<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> observe(
                String meshName, int capacity) {
            return subscriber -> {};
        }

        @Override
        public boolean isReady(String meshName) {
            return true;
        }
    }
}
