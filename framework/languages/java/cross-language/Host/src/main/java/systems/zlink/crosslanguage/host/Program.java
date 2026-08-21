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
import org.springframework.beans.factory.ObjectProvider;
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
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.BeginUserSpotJoinReq;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotCreateReq;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotDiscoveryProbeReq;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotDiscoveryProbeRes;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotJoinRes;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotProbeReq;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotProbeRes;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.spots.ZLinkSpotCreateResult;
import systems.zlink.framework.spots.ZLinkSpotManager;
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
        ObjectProvider<ZLinkRedisLocationStore> locationStoreProvider,
        ObjectProvider<ZLinkRedisRelocationStore> relocationStoreProvider) {
        // The store beans are null in modes without --redis-endpoint
        // (spot-route, channel); ObjectProvider keeps them optional so those
        // modes can still wire this configurer.
        ZLinkRedisLocationStore locationStore = locationStoreProvider.getIfAvailable();
        ZLinkRedisRelocationStore relocationStore = relocationStoreProvider.getIfAvailable();
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
            if ("user-spot-source".equals(mode) || "user-spot-target".equals(mode)) {
                // User-Spot JoinSpot (spec 15 section 4.2): an Actor created
                // through the local Entry Spot joins a fixed User Spot owned
                // by the foreign peer. The runtime alone decides whether the
                // admission leg travels as canonical command 28 (spec 51
                // section 9) -- this host never selects the transport.
                boolean target = "user-spot-target".equals(mode);
                String meshName = args.require("mesh-name");
                // Message-flow evidence lands in the host log (the framework
                // traces through SLF4J); no <event-file>.flow is produced by
                // the Java host, so no canonical-28 wire probe exists here.
                options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
                if (locationStore != null) {
                    options.addLocationStore(locationStore);
                }
                if (relocationStore != null) {
                    options.addRelocationStore(relocationStore);
                }
                var mesh = options.addRouteMesh(meshName)
                    .listen(args.require("bind-endpoint"))
                    .setRoutingId(RoutingId.from(args.require("node-rid")))
                    // The target drops to zero once its fixed Spot exists, so
                    // the source always wins the Actor's initial placement.
                    .setPlacementWeight(100);
                mesh.channelName(meshName).server();
                mesh.addRouteRequestHandler(
                    UserSpotDiscoveryProbeHandler.class,
                    UserSpotDiscoveryProbeReq.class,
                    UserSpotDiscoveryProbeRes.class);
                mesh.objects().client();
                var objects = mesh.objects().server();
                // The target registers an Entry Spot too: without it the
                // arriving Actor has no local Entry Spot placement path.
                objects.addEntrySpot(RelocationEntrySpot.class);
                objects.addActorFactory(
                    ENTRY_RELOCATION_ACTOR_TYPE,
                    RelocationActor.class,
                    RelocationActorFactory.class,
                    factory -> factory.preserveStateWith(RelocationActorAdapter.class));
                if (target) {
                    objects.addSpotFactory(
                        RelocationUserSpot.SPOT_TYPE,
                        RelocationUserSpot.class,
                        factory -> factory.disableRelocation());
                }
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
                // A direct node request names the peer RID. Declare that
                // same RID here so the pre-send classifier retains the
                // configured route while native admission is completing;
                // endpoint-only intents can otherwise remain unaddressable
                // when their monitor edge has not supplied a remote RID.
                mesh.setRoutingId(
                    RoutingId.from(args.option("node-rid", "java-spot-route-client")));
                mesh.channelName(channel).client();
                mesh.peerConnections().connect(
                    RoutingId.from(args.require("peer-rid")),
                    args.require("server-endpoint"));
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

    @Bean
    UserSpotJoinObserver userSpotJoinObserver() {
        return new UserSpotJoinObserver();
    }

    @Bean
    NodeIdentity nodeIdentity(HostArgs args) {
        return new NodeIdentity(args.option("node-rid", ""));
    }

    @Bean
    UserSpotDiscoveryProbeHandler userSpotDiscoveryProbeHandler(NodeIdentity identity) {
        return new UserSpotDiscoveryProbeHandler(identity);
    }

    @Bean(name = "userSpotJoinRunner")
    ApplicationRunner userSpotJoinRunner(
        HostArgs args,
        EventSink sink,
        UserSpotJoinObserver observer,
        ObjectProvider<ZLinkSpotManager> spotsProvider,
        ObjectProvider<ZLinkActorManager> actorsProvider,
        ObjectProvider<ZLinkActorClient> actorClientProvider,
        ObjectProvider<ZLinkRouteClient> routeClientProvider,
        ObjectProvider<ZLinkRouteMeshRuntime> meshRuntimeProvider,
        ObjectProvider<ZLinkRouteMeshRuntimeOptions> runtimeOptionsProvider) {
        return applicationArguments -> {
            String mode = args.mode();
            if ("user-spot-source".equals(mode)) {
                Thread worker = new Thread(
                    () -> runUserSpotSource(
                        args,
                        sink,
                        actorsProvider.getObject(),
                        actorClientProvider.getObject(),
                        meshRuntimeProvider.getObject()),
                    "user-spot-source");
                worker.setDaemon(true);
                worker.start();
                return;
            }
            if ("user-spot-target".equals(mode)) {
                runUserSpotTarget(
                    args,
                    sink,
                    observer,
                    spotsProvider.getObject(),
                    actorClientProvider.getObject(),
                    routeClientProvider.getObject(),
                    meshRuntimeProvider.getObject(),
                    runtimeOptionsProvider.getObject());
            }
        };
    }

    private static void runUserSpotSource(
        HostArgs args,
        EventSink sink,
        ZLinkActorManager actors,
        ZLinkActorClient actorClient,
        ZLinkRouteMeshRuntime meshRuntime) {
        String meshName = args.require("mesh-name");
        String actorId = args.option("actor-id", "cross-lang-user-spot-actor");
        String targetSpotId = args.require("spot-id");
        try {
            long discoveryDeadline = System.nanoTime() + Duration.ofSeconds(60).toNanos();
            while (meshRuntime.snapshot(meshName).readyPeerCount() == 0) {
                if (System.nanoTime() >= discoveryDeadline) {
                    sink.append("user-spot-source-peer-ready|ready=false|reason=discovery-timeout");
                    return;
                }
                sleepQuietly(100);
            }
            sink.append("user-spot-source-peer-ready|ready=true");

            // Optional reciprocal barrier: the harness touches this file once
            // BOTH sides have observed the peer, matching the Node source.
            String startFile = args.option("start-file", null);
            if (startFile != null && !startFile.isBlank()) {
                Path path = Path.of(startFile);
                long startDeadline = System.nanoTime() + Duration.ofSeconds(60).toNanos();
                while (!Files.exists(path)) {
                    if (System.nanoTime() >= startDeadline) {
                        sink.append("user-spot-source-start-timeout|file=" + startFile);
                        return;
                    }
                    sleepQuietly(25);
                }
            }

            ZLinkActorCreateResult created = actors
                .getOrCreate(actorId, ENTRY_RELOCATION_ACTOR_TYPE)
                .inMesh(meshName)
                .request(new CrossLangActorCreateReq(7, 4))
                .timeout(Duration.ofSeconds(15))
                .submit()
                .toCompletableFuture()
                .get(20, TimeUnit.SECONDS);
            String createStatus = switch (created) {
                case ZLinkActorCreateResult.Created ignored -> "created";
                case ZLinkActorCreateResult.Existing ignored -> "existing";
                case ZLinkActorCreateResult.Rejected ignored -> "rejected";
            };
            String ownerNode = switch (created) {
                case ZLinkActorCreateResult.Created value -> value.actor().nodeRid().toString();
                case ZLinkActorCreateResult.Existing value -> value.actor().nodeRid().toString();
                default -> "none";
            };
            sink.append("user-spot-source-actor-created|status=" + createStatus
                + "|node=" + ownerNode);

            UserSpotJoinRes reply = actorClient
                .requestToActor(actorId, new BeginUserSpotJoinReq(targetSpotId, "canonical-28"))
                .timeout(Duration.ofSeconds(45))
                .submit(UserSpotJoinRes.class)
                .toCompletableFuture()
                .get(50, TimeUnit.SECONDS);
            sink.append("user-spot-join-request-reply|accepted=" + reply.accepted()
                + "|actor=" + reply.actorId() + "|spot=" + reply.spotId());
        } catch (ExecutionException | TimeoutException error) {
            sink.append("user-spot-source-error|" + describe(error));
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
        }
    }

    private static void runUserSpotTarget(
        HostArgs args,
        EventSink sink,
        UserSpotJoinObserver observer,
        ZLinkSpotManager spots,
        ZLinkActorClient actorClient,
        ZLinkRouteClient routes,
        ZLinkRouteMeshRuntime meshRuntime,
        ZLinkRouteMeshRuntimeOptions runtimeOptions) {
        String meshName = args.require("mesh-name");
        String spotId = args.require("spot-id");
        String actorId = args.option("actor-id", "cross-lang-user-spot-actor");
        String sourceNodeRid = args.require("peer-rid");
        try {
            ZLinkSpotCreateResult created = spots
                .getOrCreate(spotId, RelocationUserSpot.SPOT_TYPE)
                .inMesh(meshName)
                .request(new UserSpotCreateReq("cross-language-user-spot"))
                .timeout(Duration.ofSeconds(15))
                .submit()
                .toCompletableFuture()
                .get(20, TimeUnit.SECONDS);
            String targetNodeRid = created.spot().nodeRid().toString();
            // The fixed target Spot exists now. Exclude this node from the
            // source Actor's Entry-Spot placement so the join is a real
            // cross-node admission rather than a local self-join.
            runtimeOptions.mesh(meshName).setPlacementWeight(0);
            sink.append("user-spot-created|spot=" + created.spot().spotId()
                + "|nodeRid=" + targetNodeRid + "|state=" + created.state().name());
            // ApplicationRunners run before main() writes the ready file, and
            // the source only starts after it appears; write it here.
            writeReadyFile(args);
            startDaemon("user-spot-target-discovery",
                () -> observeSourcePeer(sink, routes, meshRuntime, meshName, sourceNodeRid));
            startDaemon("user-spot-target-probe",
                () -> probeJoinedActor(sink, observer, actorClient, actorId, targetNodeRid));
        } catch (ExecutionException | TimeoutException error) {
            sink.append("user-spot-target-error|" + describe(error));
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
        }
    }

    private static void observeSourcePeer(
        EventSink sink,
        ZLinkRouteClient routes,
        ZLinkRouteMeshRuntime meshRuntime,
        String meshName,
        String sourceNodeRid) {
        long deadlineNanos = System.nanoTime() + Duration.ofSeconds(120).toNanos();
        while (System.nanoTime() < deadlineNanos) {
            int readyPeers = meshRuntime.snapshot(meshName).readyPeerCount();
            if (readyPeers > 0) {
                try {
                    UserSpotDiscoveryProbeRes reply = routes
                        .requestToNode(
                            meshName,
                            RoutingId.from(sourceNodeRid),
                            new UserSpotDiscoveryProbeReq("reciprocal-discovery"))
                        .timeout(Duration.ofSeconds(2))
                        .submit(UserSpotDiscoveryProbeRes.class)
                        .toCompletableFuture()
                        .get(4, TimeUnit.SECONDS);
                    if (sourceNodeRid.equals(reply.nodeRid())) {
                        sink.append("user-spot-source-peer-ready|ready=true|peers=" + readyPeers);
                        return;
                    }
                } catch (ExecutionException | TimeoutException ignored) {
                    // Peer edge not admitted in both directions yet.
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
            sleepQuietly(100);
        }
        sink.append("user-spot-source-peer-ready|ready=false|reason=probe-timeout");
    }

    private static void probeJoinedActor(
        EventSink sink,
        UserSpotJoinObserver observer,
        ZLinkActorClient actorClient,
        String actorId,
        String targetNodeRid) {
        try {
            observer.joined().get(75, TimeUnit.SECONDS);
        } catch (ExecutionException | TimeoutException error) {
            sink.append("user-spot-probe-timeout|targetRid=" + targetNodeRid
                + "|failure=join-not-observed");
            return;
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            return;
        }
        long deadlineNanos = System.nanoTime() + Duration.ofSeconds(90).toNanos();
        String lastFailure = "none";
        while (System.nanoTime() < deadlineNanos) {
            try {
                UserSpotProbeRes reply = actorClient
                    .requestToActor(actorId, new UserSpotProbeReq("target-owner-probe"))
                    .timeout(Duration.ofSeconds(5))
                    .submit(UserSpotProbeRes.class)
                    .toCompletableFuture()
                    .get(7, TimeUnit.SECONDS);
                if (targetNodeRid.equals(reply.nodeRid())) {
                    sink.append("user-spot-probe|nodeRid=" + reply.nodeRid()
                        + "|targetRid=" + targetNodeRid
                        + "|actor=" + reply.actorId()
                        + "|stateVersion=" + reply.stateVersion());
                    return;
                }
                lastFailure = "unexpected-owner:" + reply.nodeRid();
            } catch (ExecutionException | TimeoutException error) {
                lastFailure = describe(error);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                return;
            }
            sleepQuietly(500);
        }
        sink.append("user-spot-probe-timeout|targetRid=" + targetNodeRid
            + "|failure=" + lastFailure);
    }

    private static void startDaemon(String name, Runnable body) {
        Thread thread = new Thread(body, name);
        thread.setDaemon(true);
        thread.start();
    }

    @Bean(name = "entryRelocationRunner")
    ApplicationRunner entryRelocationRunner(
        HostArgs args,
        EventSink sink,
        ObjectProvider<ZLinkActorManager> actorsProvider,
        ObjectProvider<ZLinkActorClient> actorClientProvider,
        ObjectProvider<ZLinkRouteMeshRuntimeOptions> runtimeOptionsProvider,
        ZLinkFrameworkLifecycle lifecycle) {
        // Actor beans only exist in the entry-spot modes (the framework
        // exposes them when actor factories are registered); keep them
        // optional so the spot-route/channel modes can still boot.
        return applicationArguments -> {
            String mode = args.mode();
            if ("entry-spot-source".equals(mode)) {
                runEntryRelocationSource(args, sink, actorsProvider.getObject(), lifecycle);
            } else if ("entry-spot-target".equals(mode)) {
                runEntryRelocationTarget(
                    args, sink, actorClientProvider.getObject(), runtimeOptionsProvider.getObject());
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
        HostArgs args,
        EventSink sink,
        ZLinkActorClient actorClient,
        ZLinkRouteMeshRuntimeOptions runtimeOptions) {
        // ApplicationRunners execute inside SpringApplication.run(), BEFORE
        // main() writes the ready file -- but this runner polls for up to 60s
        // for a probe that the harness only triggers AFTER seeing the ready
        // file. Write it up front so the source side can start.
        writeReadyFile(args);
        String actorId = args.option("actor-id", "cross-lang-relocation-actor");
        String nodeRid = args.require("node-rid");
        // Keep the target out of the source's initial create turn, then make
        // it eligible before the source's first five-second relocation
        // attempt. Waiting for a routed probe to do this deadlocks when the
        // foreign source correctly excludes a zero-weight target.
        sleepQuietly(3000);
        runtimeOptions.mesh(args.require("mesh-name")).setPlacementWeight(100);
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
