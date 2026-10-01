package systems.zlink.framework.perf.servers.channel;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.configuration.ClientServerChannelBuilder;
import systems.zlink.framework.monitoring.ZLinkClientServerRuntime;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.CompletionLoop;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.Polling;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.perf.Streams;

import java.net.URI;
import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLongArray;
import java.util.concurrent.atomic.AtomicReference;

// §11.2 channel-echo-only: two Channel processes, manual RouteMesh or ClientServer, no Store/objects.
// Source public request -> typed identity/full-byte validation is one operation.
// JSON payloads: 1024/4096, request/ordinary. Connector/Actor/Spot/worker/fanout metrics do not apply.
public final class ChannelEchoOnlyScenario {
    private final RoleConfig config;
    private final ZLinkRouteClient client;
    private final Measurement measurement;
    private final ZLinkRouteMeshRuntime meshRuntime;
    private final ZLinkClientServerRuntime channelRuntime;
    private final ObjectsReadiness readiness;
    private AtomicLongArray sequences = new AtomicLongArray(0);

    public ChannelEchoOnlyScenario(ZLinkRouteClient client, Measurement measurement, ZLinkRouteMeshRuntime meshRuntime,
            ZLinkClientServerRuntime channelRuntime, ObjectsReadiness readiness) {
        this.config = measurement.config();
        this.client = client;
        this.measurement = measurement;
        this.meshRuntime = meshRuntime;
        this.channelRuntime = channelRuntime;
        this.readiness = readiness;
    }

    /** The role host (§6.3): manual RouteMesh peer or ClientServer client/server, no Store and no objects. */
    public static void run(RoleConfig config) {
        if (!"channel".equals(config.role())) {
            throw new IllegalArgumentException("channel-server supports the channel-echo-only source and target.");
        }
        ServerApplication app = ServerApplication.create(config).configure(options -> {
            if ("routemesh".equals(config.topology())) {
                ZLinkMeshNodeBuilder mesh = ServerApplication.routeMesh(options, config, "perf-channel");
                if (config.source()) {
                    mesh.channelName(config.channelName()).client();
                    mesh.peerConnections().connect(config.peerEndpoint());
                } else {
                    mesh.channelName(config.channelName()).server()
                            .addRequestHandler(ChannelEchoHandler.class, PerfEchoRequest.class, PerfEchoReply.class);
                }
            } else if ("clientserver".equals(config.topology())) {
                ClientServerChannelBuilder channel = options.addClientServerChannel(config.channelName());
                if (config.source()) {
                    channel.client().connect(config.peerEndpoint());
                } else {
                    channel.server().listen(URI.create(config.listenerEndpoint()).getPort())
                            .addRequestHandler(ChannelEchoHandler.class, PerfEchoRequest.class, PerfEchoReply.class);
                }
            } else {
                throw new IllegalArgumentException("Unsupported channel topology.");
            }
        });
        if (config.source()) {
            app.bean(ObjectsReadiness.class, () -> new ObjectsReadiness(false, "The Channel target probe has not completed."));
            app.bean(ChannelEchoOnlyScenario.class).workload(ChannelEchoOnlyScenario.class, ChannelEchoOnlyScenario::run);
        }
        var context = app.start();
        if (config.source()) {
            context.getBean(ChannelEchoOnlyScenario.class).prepare();
        }
    }

    public CompletionStage<Void> prepare() {
        AtomicReference<Object> channelTargetStatus = new AtomicReference<>();
        // The observe stream is a change stream, not an initial snapshot (monitoring §6): query the public status until the
        // setup evidence is ready; the probe call is never retried.
        return Polling.until(() -> {
            if ("routemesh".equals(config.topology())) {
                var status = meshRuntime.snapshot(config.meshName());
                var channel = status.channels().stream().filter(item -> item.channelName().equals(config.channelName())).findFirst().orElse(null);
                if (status.isReady() && channel != null && channel.isReady() && channel.readyTargetCount() > 0) {
                    channelTargetStatus.set(channel);
                    return true;
                }
                return false;
            }
            var status = channelRuntime.snapshot(config.channelName());
            if (status.isReady() && status.readyTargetCount() > 0) {
                channelTargetStatus.set(status);
                return true;
            }
            return false;
        }, 5, config.workload().setupTimeoutMs()).thenCompose(ignored -> {
            sequences = new AtomicLongArray(config.workload().logicalStreams());
            PerfEchoRequest request = measurement.request(0, sequences.incrementAndGet(0), true);
            return client.requestToChannel(config.channelName(), request)
                    .timeout(Duration.ofMillis(config.workload().requestTimeoutMs()))
                    .submit(PerfEchoReply.class)
                    .thenAccept(reply -> {
                        PayloadPattern.validateIdentity(request, reply);
                        measurement.pattern().validate(reply.payload());
                        Map<String, Object> observed = new LinkedHashMap<>();
                        observed.put("correlationId", request.correlationId());
                        observed.put("receivedTicks", reply.receivedTicks());
                        observed.put("clockDomainId", reply.clockDomainId());
                        measurement.setupEvidence(List.of(Evidence.of("typedProbeEcho",
                                "ZLinkRouteClient.requestToChannel.submit(PerfEchoReply.class)", observed)));
                        readiness.set(true, "", List.of(Evidence.of("channelTarget",
                                "routemesh".equals(config.topology()) ? "ZLinkRouteMeshRuntime.snapshot" : "ZLinkClientServerRuntime.snapshot",
                                channelTargetStatus.get())));
                    });
        }).exceptionally(error -> {
            measurement.recordDiagnostic(error);
            return null;
        });
    }

    public CompletionStage<Void> run() {
        return Streams.launch(config, this::step);
    }

    private void step(int stream, CompletableFuture<Void> done) {
        CompletionLoop.run(done, () -> {
            if (!measurement.canIssue()) {
                return Optional.empty();
            }
            PerfEchoRequest request = measurement.request(stream, sequences.incrementAndGet(stream), false);
            long started = measurement.beginOperation();
            if (started < 0) {
                return Optional.empty();
            }
            PerfEchoRequest sent = request.withSentTicks(started);
            CompletionStage<PerfEchoReply> call;
            try {
                call = client.requestToChannel(config.channelName(), sent)
                        .timeout(Duration.ofMillis(config.workload().requestTimeoutMs()))
                        .submit(PerfEchoReply.class);
            } catch (RuntimeException error) {
                call = CompletableFuture.failedFuture(error);
            }
            return Optional.of(new CompletionLoop.Iteration<>(call, (reply, error) -> {
                if (error == null) {
                    try {
                        PayloadPattern.validateIdentity(sent, reply);
                        measurement.pattern().validate(reply.payload());
                        measurement.completeOperation(started);
                    } catch (RuntimeException invalid) {
                        measurement.completeOperation(started, invalid);
                    }
                } else {
                    measurement.completeOperation(started, error);
                }
            }));
        });
    }
}
