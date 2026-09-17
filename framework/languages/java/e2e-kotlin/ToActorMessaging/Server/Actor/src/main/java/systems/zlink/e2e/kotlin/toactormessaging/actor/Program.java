package systems.zlink.e2e.kotlin.toactormessaging.actor;
import java.util.List;
import java.util.Map;

import org.springframework.boot.ApplicationRunner;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.annotation.Bean;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.kotlin.toactormessaging.shared.Contracts;
import systems.zlink.e2e.kotlin.toactormessaging.shared.Env;
import systems.zlink.e2e.kotlin.toactormessaging.shared.EvidenceStore;
import systems.zlink.e2e.kotlin.toactormessaging.shared.JsonHttp;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreValue;
import systems.zlink.framework.locationprovider.ZLinkStoreVersion;
import systems.zlink.framework.locationprovider.ZLinkStoreVersionCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteConflict;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.framework.ZLinkMessageContext;

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        Env.configure(args);
        boot("main builder");
        SpringApplicationBuilder builder = new SpringApplicationBuilder(Program.class)
            .web(WebApplicationType.NONE);
        boot("main keepAlive");
        builder.application().setKeepAlive(true);
        boot("main run");
        builder.run();
        boot("main run done");
    }

    @Bean
    EvidenceStore evidenceStore() {
        boot("evidenceStore");
        return new EvidenceStore();
    }

    @Bean(destroyMethod = "close")
    ZLinkRedisLocationStore locationStore() {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(Env.get("redisLocationEndpoint"))
            .setKeyPrefix(Env.get("locationKeyPrefix")));
    }

    @Bean(destroyMethod = "close")
    JsonHttp http(EvidenceStore evidence, ZLinkActorManager actors, ZLinkRedisLocationStore locations) {
        boot("http create");
        JsonHttp http = new JsonHttp(Env.get("actorHttpEndpoint"));
        boot("http route health");
        http.get("/health", () -> Map.of("status", "ok"));
        boot("http route evidence");
        http.get("/evidence", evidence::all);
        boot("http route ensure");
        http.post("/ensure", Contracts.ActorCallReq.class, request -> {
            actors.getOrCreate(request.actorId(), Contracts.ACTOR_TYPE)
                .request(ZLinkMessage.of(new Contracts.ActorCreateReq("create")))
                .submit()
                .toCompletableFuture()
                .join();
            return Contracts.ActorCallRes.ok(request.scenario(), request.actorId(), "ensured");
        });
        boot("http route fault stale");
        http.post("/fault/stale", Contracts.ActorFaultReq.class, request -> {
            ZLinkStoreValue live = ensureLiveAuthority(
                actors, locations, request.actorId());
            ZLinkStoreKey key = actorAuthorityKey(request.actorId());
            var result = locations.write(
                    new ZLinkStoreWriteRequest(
                        List.of(new ZLinkStoreVersionCondition(key, staleVersion(live))),
                        List.of(new ZLinkStorePut(
                            key, live.bytes(), remainingRetention(live)))),
                    () -> false)
                .toCompletableFuture().join();
            if (!(result instanceof ZLinkStoreWriteConflict)) {
                throw new IllegalStateException(
                    "stale authority CAS was not rejected actorId=" + request.actorId());
            }
            return Contracts.ActorCallRes.ok(
                request.scenario(), request.actorId(), "stale-rejected");
        });
        boot("http route fault route");
        http.post("/fault/route-disconnected", Contracts.ActorFaultReq.class, request -> {
            ZLinkStoreValue live = ensureLiveAuthority(
                actors, locations, request.actorId());
            ZLinkStoreKey key = actorAuthorityKey(request.actorId());
            var result = locations.write(
                    new ZLinkStoreWriteRequest(
                        List.of(new ZLinkStoreVersionCondition(key, staleVersion(live))),
                        List.of(new ZLinkStoreDelete(key))),
                    () -> false)
                .toCompletableFuture().join();
            if (!(result instanceof ZLinkStoreWriteConflict)) {
                throw new IllegalStateException(
                    "stale authority delete was not rejected actorId=" + request.actorId());
            }
            return Contracts.ActorCallRes.ok(
                request.scenario(), request.actorId(), "route-delete-rejected");
        });
        boot("http route fault restore");
        http.post("/fault/restore", Contracts.ActorFaultReq.class, request -> {
            ensureLiveAuthority(actors, locations, request.actorId());
            return Contracts.ActorCallRes.ok(request.scenario(), request.actorId(), "restored");
        });
        boot("http start");
        http.start();
        boot("http start done");
        return http;
    }

    @Bean
    ZLinkFrameworkConfigurer framework(ZLinkRedisLocationStore locationStore) {
        boot("framework configurer bean");
        return options -> {
            boot("configureDispatch");
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            boot("configureDispatch done");
            boot("addLocationStore");
            options.addLocationStore(locationStore);
            boot("addLocationStore done");
            boot("addRouteMesh");
            var spotMesh = options.addRouteMesh(Contracts.SPOT_MESH);
            boot("addRouteMesh done");
            boot("listen");
            spotMesh.listen(Env.get("actorSpotEndpoint"));
            boot("listen done");
            boot("setRoutingId");
            spotMesh.setRoutingId(RoutingId.from(Env.get("actorRid", "actor-a")));
            boot("setRoutingId done");
            boot("addEntrySpot");
            var objects = spotMesh.objects().server();
            objects.addEntrySpot(TestEntrySpot.class);
            boot("addEntrySpot done");
            boot("addActorFactory");
            objects.addActorFactory(
                Contracts.ACTOR_TYPE,
                TestActor.class,
                TestActorFactory.class,
                factory -> factory.recreateOnRelocation());
            boot("addActorFactory done");
        };
    }

    //  The Actor authority row's logical key preimage is fixed by the
    //  cross-language Location Store contract (framework spec
    //  server/05-location-relocation/01-location-runtime, "Logical key
    //  preimage"): authority\0{actor|spot}\0{Id}.
    private static ZLinkStoreKey actorAuthorityKey(String actorId) {
        if (actorId == null || actorId.isBlank() || actorId.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("actorId is required");
        }
        return new ZLinkStoreKey("authority\0actor\0" + actorId);
    }

    //  A store version that cannot match the live row, so the write must conflict.
    private static ZLinkStoreVersion staleVersion(ZLinkStoreValue live) {
        return new ZLinkStoreVersion("stale-" + live.version().value());
    }

    //  Keeps the row's remaining lifetime, so the probe never extends the lease.
    private static Duration remainingRetention(ZLinkStoreValue live) {
        if (live.expiresAt() == null) {
            return null;
        }
        Duration remaining = Duration.between(live.storeNow(), live.expiresAt());
        return remaining.isNegative() ? Duration.ZERO : remaining;
    }

    private static ZLinkStoreValue ensureLiveAuthority(
        ZLinkActorManager actors,
        ZLinkRedisLocationStore locations,
        String actorId) {
        actors.getOrCreate(actorId, Contracts.ACTOR_TYPE)
            .request(ZLinkMessage.of(new Contracts.ActorCreateReq("create")))
            .submit()
            .toCompletableFuture()
            .join();
        return waitForActorAuthority(locations, actorId);
    }

    private static ZLinkStoreValue waitForActorAuthority(
        ZLinkRedisLocationStore locations,
        String actorId) {
        long deadline = System.nanoTime() + 5_000_000_000L;
        while (System.nanoTime() < deadline) {
            var result = locations.read(
                    actorAuthorityKey(actorId),
                    () -> false)
                .toCompletableFuture().join();
            if (result instanceof ZLinkStoreReadFound found) {
                return found.value();
            }
            sleepBriefly();
        }
        throw new IllegalStateException(
            "actor authority was not published actorId=" + actorId);
    }

    private static void sleepBriefly() {
        try {
            Thread.sleep(100);
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("actor location wait interrupted", ex);
        }
    }

    @Bean
    ApplicationRunner createBaselineActors(ZLinkActorManager actors) {
        return ignored -> {
            boot("baselineActors start");
            for (String actorId : List.of("ta-a1", "ta-a2", "ta-a3", "ta-a4", "ta-b2", "ta-b3")) {
                boot("baselineActors getOrCreate actorId=" + actorId);
                actors.getOrCreate(actorId, Contracts.ACTOR_TYPE)
                    .request(ZLinkMessage.of(new Contracts.ActorCreateReq("create")))
                    .submit()
                    .toCompletableFuture()
                    .join();
                boot("baselineActors getOrCreate done actorId=" + actorId);
            }
            boot("baselineActors done");
        };
    }

    private static void boot(String step) {
        System.out.println("[boot] role=actor step=" + step);
    }

    public static final class TestActor implements ZLinkActor {
        private final ZLinkActorContext context;

        TestActor(ZLinkActorContext context) {
            this.context = context;
        }

        String actorId() { return context.actorId(); }
        @Override public ZLinkActorContext context() { return context; }
    }

    public static final class TestActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new TestActor(context));
        }
    }

    public static final class TestEntrySpot implements ZLinkEntrySpot<TestActor> {
        private final ZLinkEntrySpotContext context;
        private final EvidenceStore evidence;

        public TestEntrySpot(ZLinkEntrySpotContext context, EvidenceStore evidence) {
            this.context = context;
            this.evidence = evidence;
        }

        @Override public ZLinkEntrySpotContext context() { return context; }

        @Override
        public void configure() {
            context.handlers().addHandler(NotifyHandler.class);
            context.handlers().addHandler(AskHandler.class);
        }

        //  Entry Spot admission is decided once, on first creation. The
        //  previous generation split that decision over onCreateActor and
        //  onActorJoin; onActorJoin now belongs to User Spots only, so the
        //  single hook that owns the decision writes both evidence rows.
        @Override
        public CompletionStage<ZLinkActorCreateResponse> onCreateActor(
            TestActor actor,
            ZLinkMessage createRequest) {
            evidence.append(new Contracts.ActorEvidence("create", actor.actorId(), "create", "created"));
            evidence.append(new Contracts.ActorEvidence("admission", actor.actorId(), "join", "accepted"));
            return CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(TestActor actor) {
            evidence.append(new Contracts.ActorEvidence("join", actor.actorId(), "join", "joined"));
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(TestActor actor) {
            evidence.append(new Contracts.ActorEvidence("leave", actor.actorId(), "join", "left"));
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class NotifyHandler
        implements ZLinkEntrySpotActorSendHandler<TestEntrySpot, TestActor, Contracts.ActorMsg> {
        private final EvidenceStore evidence;

        public NotifyHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(
            TestEntrySpot entrySpot,
            TestActor actor,
            ZLinkMessageContext context,
            Contracts.ActorMsg message) {
            evidence.append(new Contracts.ActorEvidence(message.scenario(), actor.actorId(), "send", message.value()));
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class AskHandler
        implements ZLinkEntrySpotActorRequestHandler<
            TestEntrySpot,
            TestActor,
            Contracts.ActorReq,
            Contracts.ActorRes> {
        private final EvidenceStore evidence;

        public AskHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.ActorRes> handle(
            TestEntrySpot entrySpot,
            TestActor actor,
            ZLinkMessageContext context,
            Contracts.ActorReq request) {
            evidence.append(new Contracts.ActorEvidence(request.scenario(), actor.actorId(), "request", request.value()));
            return CompletableFuture.completedFuture(new Contracts.ActorRes(
                request.scenario(), actor.actorId(), "reply:" + request.value()));
        }
    }
}
