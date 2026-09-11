/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.bench.withgrpc.shared.BenchHttpApplication;
import systems.zlink.bench.withgrpc.shared.RawWire;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;

/** One Java source-A process for one implementation/pattern/payload cell. */
public final class BenchClient {
    private BenchClient() {
    }

    public static void main(String[] args) throws Exception {
        BenchOptions options = new BenchOptions(args);
        if (!java.util.List.of("grpc-java", "zlink-java", "zlink-framework-java")
            .contains(options.implementation)) {
            throw new IllegalArgumentException("unknown Java implementation");
        }

        if ("request-window".equals(options.scenario)) {
            throw new IllegalArgumentException("request-window is not a Java gRPC comparison pattern");
        }

        BenchDrivers drivers = new BenchDrivers(options);
        AtomicBoolean ready = new AtomicBoolean(false);
        SourceTransport[] transport = new SourceTransport[1];
        BenchHttpApplication.Controller[] holder = new BenchHttpApplication.Controller[1];
        BenchHttpApplication.Controller controller = new BenchHttpApplication.Controller(
            ready::get,
            drivers::counters,
            trigger -> {
                if ("warmup".equals(trigger.phase())) {
                    drivers.runWarmup(trigger, transport[0].operation());
                    return;
                }
                Map<String, Object> result = drivers.runActive(trigger, transport[0].operation());
                BenchResultWriter.write(options, holder[0].lastTrigger(), result, "java",
                    streamImplementation(options), javaMetadata(options));
            });
        holder[0] = controller;
        BenchHttpApplication http = BenchHttpApplication.start(
            options.triggerUrl, options.statsUrl, controller);
        try {
            transport[0] = createTransport(options);
            drivers.waitForRouteReady(
                transport[0].operation(), options.payloadSizes.get(0));
            ready.set(true);
        } catch (Throwable error) {
            http.close();
            throw error;
        }

        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            ready.set(false);
            http.close();
            try {
                if (transport[0] != null) {
                    transport[0].close();
                }
            } catch (Exception error) {
                System.err.println("[source] close failed: " + error);
            }
        }, "bench-source-shutdown"));
        System.err.println("[source] implementation=" + options.implementation
            + " trigger=" + options.triggerUrl + " stats=" + options.statsUrl
            + " target=" + options.targetEndpoint);
        Thread.currentThread().join();
    }

    private static SourceTransport createTransport(BenchOptions options) throws Exception {
        boolean send = "send-saturation".equals(options.scenario);
        return switch (options.implementation) {
            case "grpc-java" -> {
                GrpcStack stack = new GrpcStack(options);
                yield new SourceTransport(send ? stack.command() : stack.echo(), stack);
            }
            case "zlink-java" -> {
                Context context = Zlink.createContext();
                String peer = send ? RawWire.RAW_COMMAND_SERVER_ID : RawWire.RAW_REQUEST_SERVER_ID;
                String endpoint = send ? options.targetCommandEndpoint : options.targetEndpoint;
                RawStack stack = RawStack.create(context, options,
                    "bench-source-" + ProcessHandle.current().pid(), peer, endpoint);
                yield new SourceTransport(send ? stack.send() : stack.request(), () -> {
                    stack.close();
                    context.close();
                });
            }
            case "zlink-framework-java" -> {
                FrameworkStack stack = FrameworkStack.create(options);
                yield new SourceTransport(send ? stack.send() : stack.request(), stack);
            }
            default -> throw new IllegalArgumentException("unknown implementation");
        };
    }

    private static String streamImplementation(BenchOptions options) {
        if ("zlink-java".equals(options.implementation)
            && "request-backpressure".equals(options.scenario)) {
            return "one Java platform submit thread; one request per turn, "
                + "public POLLCOMPLETION progress, uncapped replies";
        }
        if ("zlink-java".equals(options.implementation)
            && "send-saturation".equals(options.scenario)) {
            return "one Java platform submit thread; OK continues inline, "
                + "BACKPRESSURED awaits admission";
        }
        return switch (options.scenario) {
            case "request-serial" -> "one Java platform submit thread; sequential CompletableFuture";
            case "request-backpressure" ->
                "one Java platform submit thread; uncapped CompletableFuture set";
            case "send-saturation" ->
                "eight Java platform submit threads; one completion-awaited send each";
            default -> throw new IllegalArgumentException("unknown pattern");
        };
    }

    private static Map<String, Object> javaMetadata(BenchOptions options) {
        Map<String, Object> values = new LinkedHashMap<>();
        values.put("grpcServerConfiguration",
            "io.grpc.ServerBuilder.forPort defaults, plaintext IPv4 loopback");
        values.put("logicalStreamRuntime", streamImplementation(options));
        return values;
    }

    private record SourceTransport(BenchOperation operation, AutoCloseable resource)
        implements AutoCloseable {
        @Override
        public void close() throws Exception {
            resource.close();
        }
    }
}
