package systems.zlink.framework.perf;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;

import org.springframework.boot.Banner;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.support.GenericApplicationContext;

import io.micrometer.core.instrument.MeterRegistry;
import io.micrometer.core.instrument.simple.SimpleMeterRegistry;

import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.configuration.ZLinkFrameworkOptions;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.monitoring.ZLinkClientServerStatus;
import systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Supplier;

// What every role process shares (§6.1): the role config, the Framework host (Spring starter), the application
// counters and the admin/trigger HTTP endpoints of §16. No call a scenario measures lives here.
public final class ServerApplication {
    private final RoleConfig config;
    private final Measurement measurement;
    private final List<ZLinkFrameworkConfigurer> configurers = new ArrayList<>();
    private final List<Consumer<GenericApplicationContext>> registrations = new ArrayList<>();
    private volatile ConfigurableApplicationContext context;
    private volatile Supplier<CompletionStage<Void>> workload;
    private final List<HttpServer> servers = new ArrayList<>();

    private ServerApplication(RoleConfig config) {
        this.config = config;
        this.measurement = new Measurement(config, config.source());
    }

    public static RoleConfig readConfig(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0])) {
            throw new IllegalArgumentException("Server requires --config <file> only.");
        }
        RoleConfig config;
        try {
            config = PerfJson.read(Files.readString(Path.of(args[1]), StandardCharsets.UTF_8), RoleConfig.class);
        } catch (IOException error) {
            throw new IllegalArgumentException("Role config is unreadable: " + error.getMessage(), error);
        }
        if (URI.create(config.metricsUrl()).getPort() == URI.create(config.applicationTriggerUrl()).getPort()) {
            throw new IllegalArgumentException("Admin and application trigger require separate listeners.");
        }
        return config;
    }

    /** The directory of the cell: role-configs/<role>.json sits one folder below it (§15.1). */
    public static Path cellDirectory(String[] args) {
        return Path.of(args[1]).toAbsolutePath().getParent().getParent();
    }

    public static ServerApplication create(RoleConfig config) {
        ServerApplication application = new ServerApplication(config);
        application.registrations.add(ctx -> {
            ctx.registerBean(RoleConfig.class, () -> config);
            ctx.registerBean(Measurement.class, () -> application.measurement);
            ctx.registerBean(MeterRegistry.class, SimpleMeterRegistry::new);
            ctx.registerBean(PublicMetricCollector.class);
            ctx.registerBean("perfCommonConfigurer", ZLinkFrameworkConfigurer.class, () -> application::commonOptions);
        });
        return application;
    }

    public RoleConfig config() {
        return config;
    }

    public Measurement measurement() {
        return measurement;
    }

    public ConfigurableApplicationContext context() {
        return context;
    }

    /** The role's topology, handlers and objects (§6.3): its own ZLinkFrameworkOptions registration. */
    public ServerApplication configure(Consumer<ZLinkFrameworkOptions> action) {
        configurers.add(action::accept);
        return this;
    }

    public <T> ServerApplication bean(Class<T> type) {
        registrations.add(ctx -> ctx.registerBean(type));
        return this;
    }

    public <T> ServerApplication bean(Class<T> type, Supplier<T> supplier) {
        registrations.add(ctx -> ctx.registerBean(type, supplier));
        return this;
    }

    /** The workload the application trigger starts: one call of the named scenario bean (§4.2). */
    public <T> ServerApplication workload(Class<T> scenarioType, Function<T, CompletionStage<Void>> run) {
        workload = () -> run.apply(context.getBean(scenarioType));
        return this;
    }

    // Options every role registers: timeouts, loopback listeners, the run's Store and the diagnostics level (§5.2, §20).
    private void commonOptions(ZLinkFrameworkOptions options) {
        options.setDefaultRequestTimeout(Duration.ofMillis(config.workload().requestTimeoutMs()));
        options.configureNetwork().setBindHost("127.0.0.1");
        options.configureNetwork().setAdvertiseHost("127.0.0.1");
        options.configureDispatch().messageFlow(
                config.diagnostics() == null ? ZLinkMessageFlowLogMode.OFF : ZLinkMessageFlowLogMode.NORMAL);
        if (config.store() != null) {
            // Perf spec §20: the run-owned Docker Redis, one namespace per cell; only Store scenarios carry it.
            options.addLocationStore(new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
                    .setConnectionString(config.store().endpoint())
                    .setKeyPrefix(config.store().namespace() + ":")));
        }
    }

    /** An automatic RouteMesh node on this role's mesh listener. One-way sends have no send timeout (#1461). */
    public static ZLinkMeshNodeBuilder routeMesh(ZLinkFrameworkOptions options, RoleConfig config, String routingIdPrefix) {
        ZLinkMeshNodeBuilder mesh = options.addRouteMesh(config.meshName())
                .setRoutingIdPrefix(routingIdPrefix)
                .listen(config.transportEndpoints().get("mesh"));
        return mesh;
    }

    /** Starts the admin and trigger listeners, then the Framework host, and returns once the host has started. */
    public ConfigurableApplicationContext start() {
        servers.add(listen(config.metricsUrl(), Map.of(
                "/perf/ready", this::ready,
                "/perf/stats", this::stats,
                "/perf/reset", this::reset)));
        servers.add(listen(config.applicationTriggerUrl(), Map.of("/app/perf/start", this::trigger)));
        SpringApplicationBuilder builder = new SpringApplicationBuilder(PerfRoleApplication.class)
                .web(WebApplicationType.NONE)
                .bannerMode(Banner.Mode.OFF)
                .properties("logging.level.root=WARN", "spring.main.keep-alive=true", "spring.jmx.enabled=false")
                .initializers(applicationContext -> {
                    GenericApplicationContext generic = (GenericApplicationContext) applicationContext;
                    for (Consumer<GenericApplicationContext> registration : registrations) {
                        registration.accept(generic);
                    }
                    for (int index = 0; index < configurers.size(); index++) {
                        ZLinkFrameworkConfigurer configurer = configurers.get(index);
                        generic.registerBean("perfRoleConfigurer" + index, ZLinkFrameworkConfigurer.class, () -> configurer);
                    }
                });
        if (config.diagnostics() != null) {
            // Message-flow records go to the Framework's standard logger; a diagnostic run keeps them in its own file.
            builder.properties("logging.file.name=" + config.diagnostics().flowFile(), "logging.level.systems.zlink=INFO");
        }
        context = builder.run();
        measurement.samplePublicState(this::publicStatus);
        return context;
    }

    private interface Endpoint {
        void handle(HttpExchange exchange) throws IOException;
    }

    private HttpServer listen(String url, Map<String, Endpoint> endpoints) {
        URI uri = URI.create(url);
        try {
            HttpServer server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            endpoints.forEach((path, endpoint) -> server.createContext(path, exchange -> {
                try {
                    endpoint.handle(exchange);
                } catch (RuntimeException | IOException error) {
                    measurement.recordDiagnostic(error);
                    respond(exchange, 500, Map.of("reason", String.valueOf(error.getMessage()), "type", error.getClass().getName()));
                } finally {
                    exchange.close();
                }
            }));
            server.setExecutor(Executors.newFixedThreadPool(4, runnable -> {
                Thread thread = new Thread(runnable, "perf-admin");
                thread.setDaemon(true);
                return thread;
            }));
            server.start();
            return server;
        } catch (IOException error) {
            throw new IllegalStateException("Cannot listen on " + url + ": " + error.getMessage(), error);
        }
    }

    private static void respond(HttpExchange exchange, int status, Object body) {
        try {
            byte[] bytes = ServerJson.write(body).getBytes(StandardCharsets.UTF_8);
            exchange.getResponseHeaders().add("Content-Type", "application/json");
            exchange.sendResponseHeaders(status, bytes.length);
            exchange.getResponseBody().write(bytes);
        } catch (IOException | RuntimeException error) {
            // The client went away; the runner records the collection failure on its side.
        }
    }

    private static String body(HttpExchange exchange) throws IOException {
        return new String(exchange.getRequestBody().readAllBytes(), StandardCharsets.UTF_8);
    }

    private ZLinkFrameworkRuntime runtime() {
        return context.getBean(ZLinkFrameworkRuntime.class);
    }

    private void ready(HttpExchange exchange) {
        respond(exchange, 200, readiness());
    }

    private void stats(HttpExchange exchange) {
        // The runner's final read of a phase seals its originals after the measured window has ended.
        String query = exchange.getRequestURI().getRawQuery();
        measurement.finalSnapshot(query != null && (query.equals("final") || query.startsWith("final=") || query.contains("&final")));
        PerfSnapshot snapshot = measurement.snapshot(publicStatus());
        snapshot.publicMetrics = context.getBean(PublicMetricCollector.class).snapshot();
        int[] version = Zlink.version();
        snapshot.provenance.put("coreVersion", version[0] + "." + version[1] + "." + version[2]);
        respond(exchange, 200, snapshot.toMap());
    }

    private void reset(HttpExchange exchange) throws IOException {
        if (!"POST".equals(exchange.getRequestMethod())) {
            respond(exchange, 405, Map.of("reason", "POST required"));
            return;
        }
        ResetRequest request;
        try {
            request = PerfJson.read(body(exchange), ResetRequest.class);
            if (request.runId() == null || request.cellId() == null || request.resetSeq() == null) {
                throw new PerfJson.PerfJsonException("Identity text fields must be non-null JSON strings.", null);
            }
        } catch (PerfJson.PerfJsonException error) {
            respond(exchange, 400, Map.of("reason", String.valueOf(error.getMessage())));
            return;
        }
        ResetReply reply;
        try {
            reply = measurement.reset(request, () -> {
                ZLinkFrameworkRuntime runtime = runtime();
                runtime.resetCapacityMetrics();
                return runtime.status().capacity().measurementEpoch();
            });
        } catch (PerfJson.PerfJsonException error) {
            respond(exchange, 400, Map.of("reason", String.valueOf(error.getMessage())));
            return;
        }
        respond(exchange, reply.ok() ? 200 : 409, reply);
    }

    private void trigger(HttpExchange exchange) throws IOException {
        if (!"POST".equals(exchange.getRequestMethod())) {
            respond(exchange, 405, Map.of("reason", "POST required"));
            return;
        }
        PerfTriggerRequest request;
        try {
            request = PerfJson.read(body(exchange), PerfTriggerRequest.class);
            if (request.runId() == null || request.cellId() == null || request.phase() == null || request.resetSeq() == null) {
                throw new PerfJson.PerfJsonException("Identity text fields must be non-null JSON strings.", null);
            }
            DecimalText.u64(request.resetSeq());
        } catch (PerfJson.PerfJsonException | PerfValidationException error) {
            respond(exchange, 400, Map.of("reason", String.valueOf(error.getMessage())));
            return;
        }
        PerfReady ready = readiness();
        // §16.1: warmup starts after infrastructure and objects; only the measured barrier needs consumersReady (PS marker).
        boolean allowed = "warmup".equals(request.phase()) ? ready.infrastructureReady() && ready.objectsReady() : ready.ready();
        if (!allowed) {
            respond(exchange, 409, Map.of("reason", "Readiness evidence is incomplete.", "ready", ready));
            return;
        }
        PerfTriggerReply reply = measurement.start(request, workload);
        respond(exchange, reply.accepted() ? 200 : 409, reply);
    }

    private String observedTopology() {
        return config.topology();
    }

    private Object publicStatus() {
        ZLinkFrameworkRuntime runtime = runtime();
        Map<String, Object> status = new LinkedHashMap<>();
        status.put("host", runtime.status());
        String topology = observedTopology();
        if ("routemesh".equals(topology)) {
            status.put("routeMesh", runtime.routeMeshRuntime().snapshot(config.meshName()));
        } else if ("clientserver".equals(topology)) {
            status.put("clientServer", runtime.clientServerRuntime().snapshot(config.channelName()));
        }
        return status;
    }

    private PerfReady readiness() {
        List<String> reasons = new ArrayList<>();
        ConfigurableApplicationContext running = context;
        if (running == null || !running.isRunning()) {
            reasons.add("The Framework host is starting.");
            return new PerfReady(config.runId(), config.cellId(), config.role(), config.roleInstance(), false, false, false, false,
                    PerfClock.unixMs(), List.of(), reasons);
        }
        ZLinkFrameworkRuntime runtime = runtime();
        ZLinkFrameworkRuntimeStatus host = runtime.status();
        boolean infrastructure = host.isReady();
        String topology = observedTopology();
        if ("routemesh".equals(topology)) {
            ZLinkMeshNodeSnapshot mesh = runtime.routeMeshRuntime().snapshot(config.meshName());
            // Channel messaging §3: RouteMesh excludes the sending node itself from candidates. Only the source needs a
            // selectable remote target; the receiver proves dispatch by echo. A source that is itself the only Server of
            // its return ChannelName (send/send, §10.4) has no remote target by design; the coordinator states that in
            // the role config (awaitRemoteTargets=false).
            infrastructure &= mesh.isReady() && (!config.source() || !config.awaitRemoteTargets()
                    || mesh.channels().stream().anyMatch(channel -> channel.channelName().equals(config.channelName())
                            && channel.isReady() && channel.readyTargetCount() > 0));
        } else if ("clientserver".equals(topology)) {
            ZLinkClientServerStatus channel = runtime.clientServerRuntime().snapshot(config.channelName());
            infrastructure &= channel.isReady() && channel.readyTargetCount() > 0;
        }
        if ("ObjectClient".equals(config.objectRole()) && config.meshName() != null) {
            infrastructure &= runtime.routeMeshRuntime().snapshot(config.meshName()).readyPeerCount() > 0;
        }
        boolean probe = !measurement.setupEvidence().isEmpty();
        // A role without this cell's public create/bind result registers ObjectsReadiness; baselines have none.
        ObjectsReadiness objects = running.getBeanProvider(ObjectsReadiness.class).getIfAvailable();
        boolean objectsReady = objects == null || objects.ready();
        List<Object> evidence = new ArrayList<>();
        Map<String, Object> statusEvidence = new LinkedHashMap<>();
        statusEvidence.put("kind", "publicStatus");
        statusEvidence.put("source", "public Framework runtime status");
        statusEvidence.put("observedValue", publicStatus());
        evidence.add(statusEvidence);
        if (!config.transportEndpoints().isEmpty()) {
            Map<String, Object> listeners = new LinkedHashMap<>();
            listeners.put("kind", "verifiedListenerReservation");
            listeners.put("source", "role config; coordinator OS bind reservation and public host startup");
            listeners.put("observedValue", config.transportEndpoints());
            evidence.add(listeners);
        }
        if (objects != null) {
            evidence.addAll(objects.evidence());
        }
        evidence.addAll(measurement.setupEvidence());
        evidence.addAll(measurement.errorEvidence());
        if (!infrastructure) {
            reasons.add("Public host/channel/listener infrastructure is not ready.");
        }
        if (!objectsReady) {
            reasons.add(objects.reason());
        }
        if (!probe) {
            reasons.add("No successful typed probe echo has been observed.");
        }
        if (measurement.hasErrors()) {
            reasons.add("Application preparation or phase failed.");
        }
        return new PerfReady(config.runId(), config.cellId(), config.role(), config.roleInstance(), infrastructure,
                objectsReady, probe, infrastructure && objectsReady && probe && !measurement.hasErrors(), PerfClock.unixMs(),
                evidence, reasons);
    }
}
