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
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

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
    ZLinkFrameworkConfigurer crossLanguageFramework(HostArgs args) {
        return options -> {
            String mode = args.mode();
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
                // endpoint; the routing id is learned from admission. Tried
                // switching to the connect(RoutingId, endpoint) overload
                // (declaring the expected peer id up front, like the .NET
                // spot-route-client TestHost mode) to see if it would also
                // unblock Java<->Node/.NET admission -- it did not, and it
                // regressed the previously-green Java<->C++ direction
                // (stage_java_spot_route_client_cpp_host started failing
                // with kind=unavailable "route is not connected"), so this
                // stays a blind connect(endpoint). See the harness
                // convergence report for the Java<->Node/.NET admission
                // finding.
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
