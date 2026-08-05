package systems.zlink.e2e.toactormessaging.actor;

import org.springframework.boot.ApplicationRunner;
import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.toactormessaging.shared.Contracts;
import systems.zlink.e2e.toactormessaging.shared.EvidenceStore;
import systems.zlink.e2e.toactormessaging.shared.JsonHttp;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkSpotActorSendContext;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.errors.ZLinkFrameworkException;

@EnableZLinkFramework
@EnableConfigurationProperties(ActorOptions.class)
@SpringBootApplication(proxyBeanMethods = false)
public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        String config = configPath(args);
        boot("main builder");
        SpringApplicationBuilder builder = new SpringApplicationBuilder(Program.class)
            .environment(isolatedEnvironment())
            .properties("spring.config.location=" + Path.of(config).toAbsolutePath().toUri())
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
    JsonHttp http(EvidenceStore evidence, ZLinkActorManager actors, ZLinkActorClient actorClient, ActorOptions config) {
        boot("http create");
        JsonHttp http = new JsonHttp(config.actorHttpEndpoint());
        boot("http route health");
        http.get("/health", () -> java.util.Map.of("status", "ok"));
        boot("http route evidence");
        http.get("/evidence", evidence::all);
        boot("http route ensure");
        http.postAsync("/ensure", Contracts.ActorCallRequest.class, request ->
            actors.getOrCreate(request.actorId(), Contracts.ACTOR_TYPE, ZLinkMessage.of("create"))
                .thenApply(ignored -> Contracts.ActorCallResponse.ok(
                    request.scenario(), request.actorId(), "ensured")));
        http.postAsync("/ensure-ref", Contracts.ActorCallRequest.class, request ->
            actors.getOrCreate(request.actorId(), Contracts.ACTOR_TYPE, ZLinkMessage.of("create"))
                .thenApply(actor -> new Contracts.ActorRefWire(
                    actor.nodeRid().toHex(), actor.actorId(), actor.objectGeneration())));
        http.postAsync("/push", Contracts.BoundPushRequest.class, request ->
            actors.find(request.actorId()).thenCompose(found -> actorClient.requestToActor(
                    found.orElseThrow(() -> new IllegalStateException(
                        "actor was not found: " + request.actorId())),
                    request)
                .submit(Contracts.BoundPushReply.class))
                .exceptionally(error -> Contracts.BoundPushReply.failed(
                    request.actorId(), request.value(), errorKind(error))));
        http.postAsync("/destroy", Contracts.DestroyActorRequest.class, request ->
            actors.find(request.actorId()).thenCompose(found -> actorClient.requestToActor(
                    found.orElseThrow(() -> new IllegalStateException(
                        "actor was not found: " + request.actorId())),
                    request)
                .submit(Contracts.DestroyActorReply.class)));
        http.postAsync("/unbind", Contracts.UnbindActorRequest.class, request ->
            actors.find(request.actorId()).thenCompose(found -> actorClient.requestToActor(
                    found.orElseThrow(() -> new IllegalStateException(
                        "actor was not found: " + request.actorId())),
                    request)
                .submit(Contracts.UnbindActorReply.class)));
        boot("http start");
        http.start();
        boot("http start done");
        return http;
    }

    @Bean
    ZLinkFrameworkConfigurer framework(ActorOptions config) {
        boot("framework configurer bean");
        return options -> {
            boot("configureDispatch");
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(config.logDirectory() + "/actor-flow.log")
                .traceLabel("java-to-actor-actor");
            boot("configureDispatch done");
            boot("addLocationStore");
            options.addLocationStore(new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
                .setConnectionString(config.redisLocationEndpoint())
                .setKeyPrefix(config.locationKeyPrefix())));
            boot("addLocationStore done");
            boot("addRouteMesh");
            var spotMesh = options.addRouteMesh(Contracts.SPOT_MESH);
            boot("addRouteMesh done");
            boot("listen");
            spotMesh.listen(config.actorSpotEndpoint());
            boot("listen done");
            boot("setRoutingId");
            spotMesh.setRoutingId(RoutingId.from(config.actorRid()));
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

    @Bean
    ApplicationRunner createBaselineActors(ZLinkActorManager actors) {
        return ignored -> {
            boot("baselineActors start");
            CompletionStage<Void> sequence = CompletableFuture.completedFuture(null);
            for (String actorId : java.util.List.of("ta-a1", "ta-a2", "ta-a3", "ta-a4", "ta-b2", "ta-b3")) {
                sequence = sequence.thenCompose(ignoredResult -> {
                    boot("baselineActors getOrCreate actorId=" + actorId);
                    return actors.getOrCreate(actorId, Contracts.ACTOR_TYPE, ZLinkMessage.of("create"))
                        .thenAccept(actor -> boot("baselineActors getOrCreate done actorId=" + actorId));
                });
            }
            sequence.whenComplete((ignoredResult, failure) -> {
                if (failure != null) {
                    boot("baselineActors failed=" + failure);
                    return;
                }
                boot("baselineActors done");
            });
        };
    }

    private static void boot(String step) {
        System.out.println("[boot] role=actor step=" + step);
    }

    public static final class TestActor implements ZLinkActor {
        private final String actorId;
        private final ZLinkActorContext context;

        TestActor(String actorId, ZLinkActorContext context) {
            this.actorId = actorId;
            this.context = context;
        }

        @Override public String actorId() { return actorId; }
        @Override public ZLinkActorContext context() { return context; }
    }

    public static final class TestActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(String actorId, ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new TestActor(actorId, context));
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
            context.handlers().addHandler(BoundPushHandler.class);
            context.handlers().addHandler(DestroyHandler.class);
            context.handlers().addHandler(UnbindHandler.class);
        }

        @Override
        public CompletionStage<Void> onCreateActor(TestActor actor, ZLinkMessage createRequest) {
            evidence.append(new Contracts.ActorEvidence("create", actor.actorId(), "create", "created"));
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId,
            ZLinkMessage request) {
            evidence.append(new Contracts.ActorEvidence("admission", actorId, "join", "accepted"));
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
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
        implements ZLinkEntrySpotActorSendHandler<TestEntrySpot, TestActor, Contracts.ActorNotify> {
        private final EvidenceStore evidence;

        public NotifyHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(
            TestEntrySpot entrySpot,
            TestActor actor,
            ZLinkSpotActorSendContext context,
            Contracts.ActorNotify message) {
            evidence.append(new Contracts.ActorEvidence(message.scenario(), actor.actorId(), "send", message.value()));
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class AskHandler
        implements ZLinkEntrySpotActorRequestHandler<
            TestEntrySpot,
            TestActor,
            Contracts.ActorAsk,
            Contracts.ActorReply> {
        private final EvidenceStore evidence;

        public AskHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.ActorReply> handle(
            TestEntrySpot entrySpot,
            TestActor actor,
            ZLinkMessageContext context,
            Contracts.ActorAsk request) {
            evidence.append(new Contracts.ActorEvidence(request.scenario(), actor.actorId(), "request", request.value()));
            return CompletableFuture.completedFuture(new Contracts.ActorReply(
                request.scenario(), actor.actorId(), "reply:" + request.value()));
        }
    }

    public static final class BoundPushHandler implements ZLinkEntrySpotActorRequestHandler<
        TestEntrySpot,
        TestActor,
        Contracts.BoundPushRequest,
        Contracts.BoundPushReply> {
        private final EvidenceStore evidence;

        public BoundPushHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.BoundPushReply> handle(
            TestEntrySpot entrySpot,
            TestActor actor,
            ZLinkMessageContext context,
            Contracts.BoundPushRequest request) {
            actor.context().boundSession().send(new Contracts.BoundPushNotify(
                request.scenario(), actor.actorId(), request.value())).submit();
            evidence.append(new Contracts.ActorEvidence(
                request.scenario(), actor.actorId(), "bound-push", request.value()));
            return CompletableFuture.completedFuture(
                Contracts.BoundPushReply.submitted(actor.actorId(), request.value()));
        }
    }

    public static final class DestroyHandler implements ZLinkEntrySpotActorRequestHandler<
        TestEntrySpot,
        TestActor,
        Contracts.DestroyActorRequest,
        Contracts.DestroyActorReply> {
        private final EvidenceStore evidence;

        public DestroyHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.DestroyActorReply> handle(
            TestEntrySpot entrySpot,
            TestActor actor,
            ZLinkMessageContext context,
            Contracts.DestroyActorRequest request) {
            if (!actor.actorId().equals(request.actorId())) {
                return CompletableFuture.failedFuture(
                    new IllegalArgumentException("destroy request actor id mismatch"));
            }
            return entrySpot.context().destroyActor(actor).thenApply(ignored -> {
                evidence.append(new Contracts.ActorEvidence(
                    request.scenario(), actor.actorId(), "destroy", "destroyed"));
                return new Contracts.DestroyActorReply(actor.actorId(), true);
            });
        }
    }

    public static final class UnbindHandler implements ZLinkEntrySpotActorRequestHandler<
        TestEntrySpot,
        TestActor,
        Contracts.UnbindActorRequest,
        Contracts.UnbindActorReply> {
        private final EvidenceStore evidence;

        public UnbindHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.UnbindActorReply> handle(
            TestEntrySpot entrySpot,
            TestActor actor,
            ZLinkMessageContext context,
            Contracts.UnbindActorRequest request) {
            return actor.context().boundSession().disconnect().thenApply(ignored -> {
                evidence.append(new Contracts.ActorEvidence(
                    request.scenario(), actor.actorId(), "session-unbound", "disconnected"));
                return new Contracts.UnbindActorReply(actor.actorId(), true);
            });
        }
    }

    private static String errorKind(Throwable error) {
        Throwable current = error;
        while (current instanceof java.util.concurrent.CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current instanceof ZLinkFrameworkException frameworkError
            ? frameworkError.kind().name()
            : current.getClass().getSimpleName();
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank())
            throw new IllegalArgumentException("Usage: to-actor-actor --config <path>");
        return args[1];
    }

    private static StandardEnvironment isolatedEnvironment() {
        StandardEnvironment value = new StandardEnvironment();
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME);
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME);
        return value;
    }
}
