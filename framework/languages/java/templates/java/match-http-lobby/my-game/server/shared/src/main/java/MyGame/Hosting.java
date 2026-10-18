package MyGame;

import org.springframework.boot.WebApplicationType;
import org.springframework.boot.builder.SpringApplicationBuilder;

import systems.zlink.framework.configuration.*;
import systems.zlink.framework.locations.redis.*;
import systems.zlink.framework.monitoring.*;
import systems.zlink.framework.runtime.host.*;

import java.io.*;
import java.util.LinkedHashMap;
import java.util.concurrent.*;
import java.util.concurrent.Flow;
import java.util.function.Consumer;

public final class Hosting {
    private Hosting() {}

    public static void configure(
            ZLinkFrameworkOptions options,
            ZLinkRedisLocationStore locations,
            ZLinkRedisRelocationStore relocations) {
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.configureLocations().setMessageFollowDuration(Control.MESSAGE_FOLLOW);
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
    }

    public static <T> Flow.Subscription observe(
            Flow.Publisher<T> publisher, CompletableFuture<Void> ready, Consumer<T> action) {
        var subscribed = new CompletableFuture<Flow.Subscription>();
        publisher.subscribe(
                new Flow.Subscriber<>() {
                    public void onSubscribe(Flow.Subscription s) {
                        subscribed.complete(s);
                        s.request(Long.MAX_VALUE);
                    }

                    public void onNext(T item) {
                        action.accept(item);
                    }

                    public void onError(Throwable error) {
                        ready.completeExceptionally(error);
                        error.printStackTrace(System.err);
                    }

                    public void onComplete() {
                        ready.completeExceptionally(
                                new IllegalStateException(
                                        "Runtime observation ended before Serving"));
                    }
                });
        return subscribed.join();
    }

    public interface HttpEndpoint {
        String endpoint();
    }

    public static void run(Class<?> application, String[] args) throws Exception {
        if (args.length != 1)
            throw new IllegalArgumentException("Supply a node configuration path");
        System.setProperty("MyGame.config", new File(args[0]).getAbsolutePath());
        try (var app =
                new SpringApplicationBuilder(application, Stores.class)
                        .web(WebApplicationType.NONE)
                        .properties("spring.main.banner-mode=off")
                        .run()) {
            var config = app.getBean(NodeConfig.class);
            var runtime = app.getBean(ZLinkFrameworkRuntime.class);
            var ready = new CompletableFuture<Void>();
            var safe = new CompletableFuture<Void>();
            var subscription =
                    observe(
                            runtime.observe(),
                            ready,
                            observed -> {
                                var status = observed.status();
                                if (runtime.messageFlowMode() != ZLinkMessageFlowLogMode.OFF) {
                                    System.err.println(
                                            "RUNTIME state="
                                                    + status.state()
                                                    + " ready="
                                                    + status.isReady()
                                                    + " safeToShutdown="
                                                    + status.safeToShutdown()
                                                    + " relocation="
                                                    + status.relocationResult()
                                                    + " termination="
                                                    + status.terminationResult());
                                }
                                if (observed.loss().discardedTerminalCount() != 0)
                                    ready.completeExceptionally(
                                            new IllegalStateException("Startup observation lost"));
                                else if (status.state() == ZLinkFrameworkRuntimeState.SERVING)
                                    ready.complete(null);
                                else if (status.state() != ZLinkFrameworkRuntimeState.PREPARING)
                                    ready.completeExceptionally(
                                            new IllegalStateException(
                                                    "Runtime startup failed: " + status.state()));
                                if (status.state() == ZLinkFrameworkRuntimeState.RELOCATED
                                        && status.safeToShutdown()) safe.complete(null);
                            });
            try {
                ready.get(Control.HOST_DEADLINE.toSeconds(), TimeUnit.SECONDS);
                var endpoints = new LinkedHashMap<String, Object>();
                endpoints.put("id", config.id());
                endpoints.put(
                        "meshEndpoint",
                        runtime.listenerStatus(ZLinkListenerKind.ROUTE_MESH, Names.MESH)
                                .endpoint());
                endpoints.put(
                        "streamEndpoint",
                        config.streamEndpoint() == null
                                ? null
                                : runtime.listenerStatus(ZLinkListenerKind.STREAM, Names.STREAM)
                                        .endpoint());
                var http = app.getBeanProvider(HttpEndpoint.class).getIfAvailable();
                endpoints.put("httpEndpoint", http == null ? null : http.endpoint());
                System.out.println(Control.READY + Json.MAPPER.writeValueAsString(endpoints));
                var input = new BufferedReader(new InputStreamReader(System.in));
                for (String command; (command = input.readLine()) != null; ) {
                    switch (command) {
                        case Control.DRAIN -> {
                            System.out.println(Control.RELOCATION_REQUESTED);
                            var result =
                                    runtime.relocate(
                                                    new ZLinkFrameworkRelocationOptions(
                                                            ZLinkFrameworkRelocationMode
                                                                    .PLANNED_MAINTENANCE,
                                                            null,
                                                            Control.HOST_DEADLINE))
                                            .toCompletableFuture()
                                            .join();
                            if (result.outcome() != ZLinkFrameworkRelocationOutcome.RELOCATED)
                                throw new IllegalStateException(result.toString());
                            System.out.println(Control.RELOCATED);
                            safe.get(Control.SAFE_SHUTDOWN_DEADLINE.toSeconds(), TimeUnit.SECONDS);
                            System.out.println(Control.SAFE_TO_SHUTDOWN);
                        }
                        case Control.SHUTDOWN -> {
                            runtime.shutdown().toCompletableFuture().join();
                            if (runtime.status().state() != ZLinkFrameworkRuntimeState.STOPPED)
                                throw new IllegalStateException(runtime.status().toString());
                            System.out.println(Control.STOPPED);
                            return;
                        }
                        default ->
                                throw new IllegalArgumentException(
                                        "Unknown control command: " + command);
                    }
                }
            } finally {
                subscription.cancel();
            }
        }
    }
}
