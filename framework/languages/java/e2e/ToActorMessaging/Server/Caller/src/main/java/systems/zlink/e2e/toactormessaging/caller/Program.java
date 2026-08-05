package systems.zlink.e2e.toactormessaging.caller;

import java.time.Duration;
import java.nio.file.Path;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.toactormessaging.shared.Contracts;
import systems.zlink.e2e.toactormessaging.shared.JsonHttp;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorDirectory;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(CallerOptions.class)
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

    @Bean(destroyMethod = "close")
    JsonHttp http(ZLinkActorClient actors, ZLinkActorDirectory actorRefs, CallerOptions config) {
        boot("http create");
        JsonHttp http = new JsonHttp(config.callerHttpEndpoint());
        boot("http route health");
        http.get("/health", () -> java.util.Map.of("status", "ok"));
        boot("http route send");
        http.postAsync("/send", Contracts.ActorCallRequest.class, request ->
            actorRef(actorRefs, request.actorId()).thenApply(actorRef -> {
                actors.sendToActor(
                    actorRef,
                    new Contracts.ActorNotify(request.scenario(), request.actorId(), request.value())).submit();
                return Contracts.ActorCallResponse.ok(request.scenario(), request.actorId(), "sent");
            }).exceptionally(error -> failed(request, error)));
        boot("http route request");
        http.postAsync("/request", Contracts.ActorCallRequest.class, request ->
            actorRef(actorRefs, request.actorId()).thenCompose(actorRef -> actors.requestToActor(
                    actorRef,
                    new Contracts.ActorAsk(request.scenario(), request.actorId(), request.value()))
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.ActorReply.class))
                .thenApply(reply -> Contracts.ActorCallResponse.ok(
                    request.scenario(), request.actorId(), reply.value()))
                .exceptionally(error -> failed(request, error)));
        boot("http route send-ref");
        http.postAsync("/send-ref", Contracts.ActorRefCallRequest.class, request -> {
            ActorRef actorRef = toActorRef(request.actorRef());
            try {
                actors.sendToActor(
                    actorRef,
                    new Contracts.ActorNotify(request.scenario(), actorRef.actorId(), request.value())).submit();
                return CompletableFuture.completedFuture(
                    Contracts.ActorCallResponse.ok(request.scenario(), actorRef.actorId(), "sent"));
            } catch (RuntimeException error) {
                return CompletableFuture.completedFuture(failed(request, error));
            }
        });
        boot("http route request-ref");
        http.postAsync("/request-ref", Contracts.ActorRefCallRequest.class, request -> {
            ActorRef actorRef = toActorRef(request.actorRef());
            return actors.requestToActor(
                        actorRef,
                        new Contracts.ActorAsk(request.scenario(), actorRef.actorId(), request.value()))
                    .timeout(Duration.ofSeconds(5))
                    .submit(Contracts.ActorReply.class)
                .thenApply(reply -> Contracts.ActorCallResponse.ok(
                    request.scenario(), actorRef.actorId(), reply.value()))
                .exceptionally(error -> failed(request, error));
        });
        boot("http start");
        http.start();
        boot("http start done");
        return http;
    }

    @Bean
    ZLinkFrameworkConfigurer framework(CallerOptions config) {
        boot("framework configurer bean");
        return options -> {
            boot("configureDispatch");
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(config.logDirectory() + "/caller-flow.log")
                .traceLabel("java-to-actor-caller");
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
            spotMesh.listen(config.callerSpotEndpoint());
            boot("listen done");
            boot("setRoutingId");
            spotMesh.setRoutingId(RoutingId.from(config.callerRid()));
            boot("setRoutingId done");
        };
    }

    private static void boot(String step) {
        System.out.println("[boot] role=caller step=" + step);
    }

    private static Contracts.ActorCallResponse failed(
        Contracts.ActorCallRequest request,
        Throwable ex) {
        return failed(request.scenario(), request.actorId(), ex);
    }

    private static Contracts.ActorCallResponse failed(
        Contracts.ActorRefCallRequest request,
        Throwable ex) {
        return failed(request.scenario(), request.actorRef().actorId(), ex);
    }

    private static Contracts.ActorCallResponse failed(
        String scenario,
        String actorId,
        Throwable ex) {
        Throwable current = ex;
        while (current.getCause() != null) {
            if (current instanceof ZLinkFrameworkException frameworkError) {
                return Contracts.ActorCallResponse.failed(
                    scenario,
                    actorId,
                    frameworkError.kind().name());
            }
            current = current.getCause();
        }
        if (current instanceof ZLinkFrameworkException frameworkError) {
            return Contracts.ActorCallResponse.failed(
                scenario,
                actorId,
                frameworkError.kind().name());
        }
        return Contracts.ActorCallResponse.failed(scenario, actorId, current.getClass().getSimpleName());
    }

    private static CompletionStage<ActorRef> actorRef(ZLinkActorDirectory actors, String actorId) {
        return actors.find(actorId)
            .thenApply(found -> found
                .orElseThrow(() -> new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NOT_FOUND,
                    "actor ref not found: " + actorId)));
    }

    private static ActorRef toActorRef(Contracts.ActorRefWire ref) {
        return new ActorRef(RoutingId.fromHex(ref.nodeRidHex()), ref.actorId(), ref.generation());
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank())
            throw new IllegalArgumentException("Usage: to-actor-caller --config <path>");
        return args[1];
    }

    private static StandardEnvironment isolatedEnvironment() {
        StandardEnvironment value = new StandardEnvironment();
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME);
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME);
        return value;
    }
}
