package systems.zlink.e2e.toactormessaging.session;

import java.util.concurrent.CompletableFuture;
import java.nio.file.Path;
import java.util.concurrent.CompletionStage;
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
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

@EnableZLinkFramework
@EnableConfigurationProperties(SessionOptions.class)
@SpringBootApplication(proxyBeanMethods = false)
public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        String config = configPath(args);
        SpringApplicationBuilder builder = new SpringApplicationBuilder(Program.class)
            .environment(isolatedEnvironment())
            .properties("spring.config.location=" + Path.of(config).toAbsolutePath().toUri())
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        var context = builder.run();
        System.out.println("[boot] role=session rid="
            + context.getBean(SessionOptions.class).sessionRid() + " step=main run done");
    }

    @Bean
    EvidenceStore evidenceStore() {
        return new EvidenceStore();
    }

    @Bean(destroyMethod = "close")
    JsonHttp http(EvidenceStore evidence, SessionOptions config) {
        JsonHttp http = new JsonHttp(config.sessionHttpEndpoint());
        http.get("/health", () -> java.util.Map.of(
            "status", "ok", "rid", config.sessionRid()));
        http.get("/evidence", evidence::all);
        http.start();
        return http;
    }

    @Bean
    ZLinkFrameworkConfigurer framework(SessionOptions config) {
        return options -> {
            String rid = config.sessionRid();
            options.addHandlersFromPackageOf(BindActorHandler.class);
            options.addLocationStore(new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
                .setConnectionString(config.redisLocationEndpoint())
                .setKeyPrefix(config.locationKeyPrefix())));
            options.addRouteMesh(Contracts.SPOT_MESH)
                .listen(config.sessionSpotEndpoint())
                .setRoutingId(RoutingId.from(rid));
            options.addStreamNode("to-actor-" + rid)
                .bind(config.sessionStreamEndpoint())
                .enableActorDispatch()
                .registerSession(ToActorSession.class);
        };
    }

    public static final class ToActorSession implements ZLinkSession {
        private final ZLinkSessionContext context;
        private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers;
        private final EvidenceStore evidence;
        private final SessionOptions options;

        public ToActorSession(
            ZLinkSessionContext context,
            ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers,
            EvidenceStore evidence,
            SessionOptions options) {
            this.context = context;
            this.handlers = handlers;
            this.evidence = evidence;
            this.options = options;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            evidence.append(new Contracts.ActorEvidence(
                "session", context.sessionId(), "session-connected", gatewayRid()));
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            CompletionStage<Void> notified = CompletableFuture.completedFuture(null);
            for (var actor : context.actors().bound()) {
                evidence.append(new Contracts.ActorEvidence(
                    "disconnect", actor.actorId(), "actor-disconnect-start", gatewayRid()));
                notified = notified.thenCompose(ignored -> actor.notifyDisconnected().thenRun(() ->
                    evidence.append(new Contracts.ActorEvidence(
                        "disconnect", actor.actorId(), "actor-disconnected", gatewayRid()))));
            }
            return notified.thenRun(() -> evidence.append(new Contracts.ActorEvidence(
                "session", context.sessionId(), "session-disconnected", gatewayRid())));
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            evidence.append(new Contracts.ActorEvidence(
                "session", context.sessionId(), "session-error", error.toString()));
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
            ZLinkSessionDispatchContext dispatch,
            systems.zlink.framework.messaging.ZLinkMessage payload) {
            return handlers.tryHandle(context, dispatch, payload).thenCompose(handled -> {
                if (handled) {
                    return CompletableFuture.completedFuture(null);
                }
                if (context.actors().bound().size() != 1) {
                    return CompletableFuture.failedFuture(new IllegalStateException(
                        "session relay requires exactly one bound actor"));
                }
                return context.actors().bound().get(0).relay(dispatch, payload);
            });
        }

        private String gatewayRid() {
            return options.sessionRid();
        }
    }

    public static final class BindActorHandler implements ZLinkTypedSessionPacketHandler<
        ZLinkSessionContext,
        Contracts.BindActorRequest> {
        private final EvidenceStore evidence;
        private final SessionOptions options;

        public BindActorHandler(EvidenceStore evidence, SessionOptions options) {
            this.evidence = evidence;
            this.options = options;
        }

        @Override
        public Class<Contracts.BindActorRequest> messageType() {
            return Contracts.BindActorRequest.class;
        }

        @Override
        public CompletionStage<Void> handle(
            ZLinkSessionContext context,
            ZLinkSessionDispatchContext dispatch,
            Contracts.BindActorRequest request) {
            Contracts.ActorRefWire wire = request.actorRef();
            ActorRef actor = new ActorRef(
                RoutingId.fromHex(wire.nodeRidHex()), wire.actorId(), wire.generation());
            return context.actors().bindOrGet(actor).thenAccept(bound -> {
                evidence.append(new Contracts.ActorEvidence(
                    "bind", actor.actorId(), "actor-bound",
                    options.sessionRid() + ":" + context.sessionId()));
                context.client().reply(new Contracts.BindActorReply(
                    actor.actorId(), actor.nodeRid().toString(), actor.objectGeneration())).submit();
            });
        }
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank())
            throw new IllegalArgumentException("Usage: to-actor-session --config <path>");
        return args[1];
    }

    private static StandardEnvironment isolatedEnvironment() {
        StandardEnvironment value = new StandardEnvironment();
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME);
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME);
        return value;
    }
}
