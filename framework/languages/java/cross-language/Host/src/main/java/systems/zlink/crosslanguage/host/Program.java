package systems.zlink.crosslanguage.host;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import org.springframework.boot.ApplicationRunner;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.Bean;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.crosslanguage.host.SpotRouteContracts.TestHostSpotRouteFailRequest;
import systems.zlink.crosslanguage.host.SpotRouteContracts.TestHostSpotRouteMissingRequest;
import systems.zlink.crosslanguage.host.SpotRouteContracts.TestHostSpotRouteReply;
import systems.zlink.crosslanguage.host.SpotRouteContracts.TestHostSpotRouteRequest;
import systems.zlink.crosslanguage.host.EntryRelocationContracts.CrossLangActorCreateReq;
import systems.zlink.crosslanguage.host.EntryRelocationContracts.CrossLangProbeReq;
import systems.zlink.crosslanguage.host.EntryRelocationContracts.CrossLangProbeRes;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationResult;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

/**
 * Java cross-language peer host (mirrors cross_language_host.cpp,
 * node_peer_host.js, and the .NET TestHost): one Spring Boot process with a
 * mode argument that speaks the shared JSON channel envelope for the
 * cross-language spot route wire scenario. Modes:
 *   spot-route-server — hosts a route-mesh channel with the (a)/(c) request
 *     handlers; (b) is a packet no handler is ever registered for.
 *   spot-route-client — connects to a peer and runs the common (a)/(b)/(c)
 *     scenario, recording markers to the event file.
 */
@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
public final class Program {
    private static final String ENTRY_RELOCATION_ACTOR_TYPE = "cross-lang-relocation-actor-type";

    private Program() {
    }

    public static void main(String[] args) {
        HostArgs parsed = new HostArgs(args);
        ConfigurableApplicationContext context = new SpringApplicationBuilder(Program.class)
            .web(WebApplicationType.NONE)
            .initializers(applicationContext ->
                applicationContext.getBeanFactory().registerSingleton("hostArgs", parsed))
            .run();
        writeReadyFile(parsed);
        watchStopFile(parsed, context);
    }

    private static void writeReadyFile(HostArgs args) {
        String readyFile = args.option("ready-file", null);
        if (readyFile == null || readyFile.isBlank()) {
            return;
        }
        try {
            Files.writeString(Path.of(readyFile), "ready\n", StandardCharsets.UTF_8);
        } catch (IOException error) {
            throw new UncheckedIOException(error);
        }
    }

    /** Blocks the main thread until the stop file appears, then closes the
     * context — the same graceful-stop contract the .NET TestHost uses. */
    private static void watchStopFile(HostArgs args, ConfigurableApplicationContext context) {
        String stopFile = args.option("stop-file", null);
        if (stopFile == null || stopFile.isBlank()) {
            return;
        }
        Path path = Path.of(stopFile);
        try {
            while (!Files.exists(path)) {
                Thread.sleep(100);
            }
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
        } finally {
            context.close();
        }
    }

    @Bean
    EventSink eventSink(HostArgs args) {
        return new EventSink(args.option("event-file", null));
    }

    @Bean
    ZLinkRedisLocationStore relocationLocationStore(HostArgs args) {
        String redisEndpoint = args.option("redis-endpoint", null);
        if (redisEndpoint == null || redisEndpoint.isBlank()) {
            return null;
        }
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(redisEndpoint)
            .setKeyPrefix(args.option("redis-key-prefix", "zlink-cross-relocation") + ":location"));
    }

    @Bean
    ZLinkRedisRelocationStore relocationRelocationStore(HostArgs args) {
        String redisEndpoint = args.option("redis-endpoint", null);
        if (redisEndpoint == null || redisEndpoint.isBlank()) {
            return null;
        }
        return new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions()
            .setConnectionString(redisEndpoint)
            .setKeyPrefix(args.option("redis-key-prefix", "zlink-cross-relocation") + ":relocation"));
    }

    @Bean
    ZLinkFrameworkConfigurer crossLanguageFramework(
        HostArgs args,
        ZLinkRedisLocationStore locationStore,
        ZLinkRedisRelocationStore relocationStore) {
        return options -> {
            String mode = args.mode();
            if ("entry-spot-source".equals(mode) || "entry-spot-target".equals(mode)) {
                String meshName = args.require("mesh-name");
                String nodeRid = args.require("node-rid");
                if (locationStore != null) {
                    options.addLocationStore(locationStore);
                }
                if (relocationStore != null) {
                    options.addRelocationStore(relocationStore);
                }
                // Pure automatic discovery, no manual PeerConnections.connect
                // anywhere: confirmed by direct repro against a .NET peer
                // that RelocateAsync explicitly rejects a manually-connected
                // topology (ZLinkFrameworkRelocationReason.ManualTopologyUnsupported),
                // and .NET's Object-role MeshNode cannot even declare a fixed
                // own routing id. Java's own e2e (SpotActorTransfer
                // Program.java) already runs relocation this way when
                // config.automaticTopology() is set -- Java, unlike .NET,
                // tolerates a fixed routing id alongside auto-discovery, so
                // it is kept here for the owner-before/probe assertions
                // below, but no peer connection is ever manually declared.
                var mesh = options.addRouteMesh(meshName)
                    .listen(args.require("bind-endpoint"))
                    .setRoutingId(RoutingId.from(nodeRid))
                    // Force deterministic placement: the source always wins
                    // actor creation, so the pre-relocation owner assertion
                    // is meaningful rather than an accident of placement.
                    .setPlacementWeight("entry-spot-source".equals(mode) ? 100 : 0);
                mesh.channelName(meshName).server();
                mesh.objects().client();
                var objects = mesh.objects().server();
                objects.addEntrySpot(RelocationEntrySpot.class);
                objects.addActorFactory(
                    ENTRY_RELOCATION_ACTOR_TYPE,
                    RelocationActor.class,
                    RelocationActorFactory.class,
                    factory -> factory.preserveStateWith(RelocationActorAdapter.class));
                return;
            }
            if ("spot-route-server".equals(mode)) {
                String channel = args.require("channel-name");
                var mesh = options.addRouteMesh(channel)
                    .listen(args.require("server-endpoint"))
                    .setRoutingId(RoutingId.from(args.option("node-rid", "java-spot-route")));
                mesh.channelName(channel).server();
                mesh.addRouteRequestHandler(
                    SpotRouteRequestHandler.class,
                    TestHostSpotRouteRequest.class,
                    TestHostSpotRouteReply.class);
                mesh.addRouteRequestHandler(
                    SpotRouteFailRequestHandler.class,
                    TestHostSpotRouteFailRequest.class,
                    TestHostSpotRouteReply.class);
                return;
            }
            if ("spot-route-client".equals(mode)) {
                String channel = args.require("channel-name");
                String bindEndpoint = args.option("bind-endpoint", null);
                var mesh = bindEndpoint == null || bindEndpoint.isBlank()
                    ? options.addRouteMesh(channel).listen()
                    : options.addRouteMesh(channel).listen(bindEndpoint);
                // RegistryMessaging e2e convention: every route mesh member
                // exposes the channel server role, and peers connect by
                // endpoint; the routing id is learned from admission. This
                // is the same blind connect(endpoint) the C++ host uses.
                //
                // An earlier attempt to switch this to the
                // connect(RoutingId, endpoint) overload regressed
                // Java<->C++ with kind=unavailable, because
                // ZLinkJavaRawMeshNode.connectPeer(endpoint, rid) fenced the
                // peer's securityIdentity against the peer's ROUTING ID
                // instead of the plaintext-transport placeholder every
                // language actually encodes -- so a declared peer id
                // rejected every non-Java peer. That framework bug is fixed
                // (see ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);
                // the blind connect stays because it now admits against
                // every peer language on its own.
                mesh.setRoutingId(
                    RoutingId.from(args.option("node-rid", "java-spot-route-client")));
                mesh.channelName(channel).server();
                mesh.peerConnections().connect(args.require("server-endpoint"));
            }
        };
    }

    @Bean
    SpotRouteRequestHandler spotRouteRequestHandler(EventSink sink) {
        return new SpotRouteRequestHandler(sink);
    }

    @Bean
    SpotRouteFailRequestHandler spotRouteFailRequestHandler() {
        return new SpotRouteFailRequestHandler();
    }

    @Bean(name = "entryRelocationRunner")
    ApplicationRunner entryRelocationRunner(
        HostArgs args,
        EventSink sink,
        ZLinkActorManager actors,
        ZLinkActorClient actorClient,
        ZLinkFrameworkLifecycle lifecycle) {
        return applicationArguments -> {
            String mode = args.mode();
            if ("entry-spot-source".equals(mode)) {
                runEntryRelocationSource(args, sink, actors, lifecycle);
            } else if ("entry-spot-target".equals(mode)) {
                runEntryRelocationTarget(args, sink, actorClient);
            }
        };
    }

    private static void runEntryRelocationSource(
        HostArgs args, EventSink sink, ZLinkActorManager actors, ZLinkFrameworkLifecycle lifecycle) {
        String actorId = args.option("actor-id", "cross-lang-relocation-actor");
        int payloadBytes = Integer.parseInt(args.option("payload-bytes", "100000"));
        try {
            ZLinkActorCreateResult created = actors
                .getOrCreate(actorId, ENTRY_RELOCATION_ACTOR_TYPE)
                .request(new CrossLangActorCreateReq(1, payloadBytes))
                .submit()
                .toCompletableFuture()
                .get(15, TimeUnit.SECONDS);
            String createStatus = switch (created) {
                case ZLinkActorCreateResult.Created ignored -> "created";
                case ZLinkActorCreateResult.Existing ignored -> "existing";
                case ZLinkActorCreateResult.Rejected ignored -> "rejected";
            };
            sink.append("entry-spot-create|status=" + createStatus);
            String ownerBefore = switch (created) {
                case ZLinkActorCreateResult.Created c -> c.actor().nodeRid().toString();
                case ZLinkActorCreateResult.Existing e -> e.actor().nodeRid().toString();
                default -> "none";
            };
            sink.append("entry-spot-owner-before|node=" + ownerBefore);

            // No manual peerConnections().connect() means no readiness
            // signal to wait on before relocating -- give automatic
            // discovery (Location Store polling on both ends) time to
            // converge, retrying relocate() itself since an early attempt
            // can observe zero eligible peers yet (matches the retry loop
            // in node_peer_host.js's entrySpotRelocate()).
            ZLinkFrameworkRelocationResult relocation = null;
            for (int attempt = 0; attempt < 6; attempt++) {
                sleepQuietly(5000);
                relocation = lifecycle.relocate(
                        new ZLinkFrameworkRelocationOptions(
                            ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE, null, Duration.ofSeconds(30)))
                    .toCompletableFuture()
                    .get(35, TimeUnit.SECONDS);
                sink.append("relocate-attempt|attempt=" + attempt
                    + "|outcome=" + relocation.outcome().name()
                    + "|reason=" + relocation.reason().name());
                if (relocation.outcome()
                    == systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome.RELOCATED) {
                    break;
                }
            }
            sink.append("relocate-result|outcome=" + relocation.outcome().name()
                + "|reason=" + relocation.reason().name());
        } catch (ExecutionException | TimeoutException error) {
            sink.append("entry-spot-source-error|" + describe(error));
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
        }
    }

    private static void runEntryRelocationTarget(
        HostArgs args, EventSink sink, ZLinkActorClient actorClient) {
        String actorId = args.option("actor-id", "cross-lang-relocation-actor");
        String nodeRid = args.require("node-rid");
        long deadlineNanos = System.nanoTime() + Duration.ofSeconds(60).toNanos();
        CrossLangProbeRes lastReply = null;
        while (System.nanoTime() < deadlineNanos) {
            try {
                lastReply = actorClient
                    .requestToActor(actorId, new CrossLangProbeReq("post-relocate-probe"))
                    .timeout(Duration.ofSeconds(5))
                    .submit(CrossLangProbeRes.class)
                    .toCompletableFuture()
                    .get(7, TimeUnit.SECONDS);
                if (nodeRid.equals(lastReply.nodeRid())) {
                    break;
                }
            } catch (ExecutionException | TimeoutException error) {
                // Actor may still be mid-relocation or not yet created; keep polling.
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                return;
            }
            sleepQuietly(500);
        }
        if (lastReply == null || !nodeRid.equals(lastReply.nodeRid())) {
            sink.append("entry-spot-probe-timeout|last="
                + (lastReply == null ? "none" : lastReply.nodeRid()));
            return;
        }
        sink.append("entry-spot-probe|nodeRid=" + lastReply.nodeRid()
            + "|stateVersion=" + lastReply.stateVersion()
            + "|applicationStateBytes=" + lastReply.applicationStateBytes());
    }

    @Bean(name = "spotRouteClientRunner")
    ApplicationRunner spotRouteClientRunner(
        HostArgs args, EventSink sink, ZLinkRouteClient client) {
        return applicationArguments -> {
            if (!"spot-route-client".equals(args.mode())) {
                return;
            }
            runSpotRouteClientScenario(args, sink, client);
        };
    }

    private static void runSpotRouteClientScenario(
        HostArgs args, EventSink sink, ZLinkRouteClient client) {
        String channel = args.require("channel-name");
        RoutingId target = RoutingId.from(args.require("peer-rid"));
        String value = args.option("value", "java-spot-route");
        Duration deadline = Duration.ofSeconds(15);
        long deadlineNanos = System.nanoTime() + deadline.toNanos();
        TestHostSpotRouteReply reply = null;
        while (true) {
            try {
                reply = client
                    .requestToNode(channel, target, new TestHostSpotRouteRequest(value))
                    .timeout(Duration.ofSeconds(5))
                    .submit(TestHostSpotRouteReply.class)
                    .toCompletableFuture()
                    .get(7, TimeUnit.SECONDS);
                break;
            } catch (ExecutionException | TimeoutException error) {
                ZLinkFrameworkException framework = unwrap(error);
                boolean retryable = framework != null
                    && (framework.kind() == ZLinkFrameworkErrorKind.UNAVAILABLE
                        || framework.kind() == ZLinkFrameworkErrorKind.NOT_FOUND
                        || framework.kind() == ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED);
                if (retryable && System.nanoTime() < deadlineNanos) {
                    sleepQuietly(50);
                    continue;
                }
                sink.append("spot-route-error|"
                    + (framework != null ? framework.kind().toString().toLowerCase() : "unknown")
                    + "|" + describe(error));
                return;
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                return;
            }
        }
        sink.append("spot-route-reply|" + reply.value());

        recordFailure(sink, client, channel, target,
            "spot-route-missing", new TestHostSpotRouteMissingRequest(value));
        recordFailure(sink, client, channel, target,
            "spot-route-app-error", new TestHostSpotRouteFailRequest(value));
    }

    private static void recordFailure(
        EventSink sink,
        ZLinkRouteClient client,
        String channel,
        RoutingId target,
        String marker,
        Object request) {
        try {
            client.requestToNode(channel, target, request)
                .timeout(Duration.ofSeconds(5))
                .submit(TestHostSpotRouteReply.class)
                .toCompletableFuture()
                .get(7, TimeUnit.SECONDS);
            sink.append(marker + "|unexpected-success");
        } catch (ExecutionException | TimeoutException error) {
            ZLinkFrameworkException framework = unwrap(error);
            String kind = framework != null
                ? framework.kind().toString().toLowerCase()
                : "unknown";
            String origin = framework != null
                && "framework".equals(framework.metadata().get("zlink.origin"))
                ? "framework"
                : framework != null ? "application" : "unspecified";
            sink.append(marker + "|kind=" + kind + "|origin=" + origin);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
        }
    }

    private static ZLinkFrameworkException unwrap(Throwable error) {
        Throwable cause = error;
        while (cause != null) {
            if (cause instanceof ZLinkFrameworkException framework) {
                return framework;
            }
            cause = cause.getCause();
        }
        return null;
    }

    private static String describe(Throwable error) {
        Throwable cause = error;
        while (cause.getCause() != null
            && (cause instanceof ExecutionException || cause instanceof CompletionException)) {
            cause = cause.getCause();
        }
        return cause.getMessage() == null ? cause.toString() : cause.getMessage();
    }

    private static void sleepQuietly(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
        }
    }
}
