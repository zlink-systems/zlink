package MyGame;

import org.springframework.boot.WebApplicationType;
import org.springframework.boot.builder.SpringApplicationBuilder;

import systems.zlink.framework.configuration.*;
import systems.zlink.framework.locations.redis.*;
import systems.zlink.framework.monitoring.*;
import systems.zlink.framework.runtime.host.*;

import java.io.*;
import java.nio.file.Path;
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

    public static void run(Class<?> application, String[] args) throws Exception {
        run(application, args, ignored -> {});
    }

    public static void run(
            Class<?> application,
            String[] args,
            Consumer<org.springframework.context.ConfigurableApplicationContext> bootstrap)
            throws Exception {
        if (args.length != 1)
            throw new IllegalArgumentException("Supply a node configuration path");
        var config = Json.MAPPER.readValue(new File(args[0]), NodeConfig.class);
        var logDirectory = System.getenv("MYGAME_LOG_DIR");
        System.setProperty(
                "MyGame.flowFile",
                Path.of(logDirectory == null ? ".logs" : logDirectory)
                        .resolve(config.id() + ".flow.log")
                        .toAbsolutePath()
                        .toString());
        var title = GameLog.nodeTitle(config.id());
        System.out.println(title + " starting");
        try (var app =
                new SpringApplicationBuilder(application, Stores.class)
                        .web(WebApplicationType.NONE)
                        .logStartupInfo(false)
                        .properties("spring.main.banner-mode=off")
                        .initializers(
                                context ->
                                        context.getBeanFactory()
                                                .registerSingleton("config", config))
                        .run()) {
            var runtime = app.getBean(ZLinkFrameworkRuntime.class);
            var ready = new CompletableFuture<Void>();
            var safe = new CompletableFuture<Void>();
            var subscription =
                    observe(
                            runtime.observe(),
                            ready,
                            observed -> {
                                var status = observed.status();
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
                bootstrap.accept(app);
                var meshEndpoint =
                        config.meshEndpoint() == null
                                ? null
                                : runtime.listenerStatus(ZLinkListenerKind.ROUTE_MESH, Names.MESH)
                                        .endpoint();
                var streamEndpoint =
                        config.streamEndpoint() == null
                                ? null
                                : runtime.listenerStatus(ZLinkListenerKind.STREAM, Names.STREAM)
                                        .endpoint();
                var apiEndpoint =
                        meshEndpoint == null && config.apiEndpoint() != null
                                ? runtime.listenerStatus(ZLinkListenerKind.CLIENT_SERVER, Names.API)
                                        .endpoint()
                                : null;
                var endpoints = new LinkedHashMap<String, Object>();
                endpoints.put("id", config.id());
                endpoints.put("meshEndpoint", meshEndpoint);
                endpoints.put("streamEndpoint", streamEndpoint);
                endpoints.put("apiEndpoint", apiEndpoint);
                var endpoint = streamEndpoint != null ? streamEndpoint : meshEndpoint;
                if (apiEndpoint != null) endpoint = apiEndpoint;
                System.out.println(title + " ready at " + endpoint);
                System.out.println(Control.READY + Json.MAPPER.writeValueAsString(endpoints));
                var input = new BufferedReader(new InputStreamReader(System.in));
                commands:
                for (String command; (command = input.readLine()) != null; ) {
                    switch (command) {
                        case Control.DRAIN -> {
                            System.out.println(Control.RELOCATION_REQUESTED);
                            System.out.println(title + " draining: moving tables and players");
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
                            System.out.println(title + " drained: tables and players moved");
                            safe.get(Control.SAFE_DEADLINE.toSeconds(), TimeUnit.SECONDS);
                            System.out.println(Control.SAFE_TO_SHUTDOWN);
                            System.out.println(title + " safe to shut down");
                        }
                        case Control.SHUTDOWN -> {
                            System.out.println(title + " stopping");
                            runtime.shutdown().toCompletableFuture().join();
                            if (runtime.status().state() != ZLinkFrameworkRuntimeState.STOPPED)
                                throw new IllegalStateException(runtime.status().toString());
                            System.out.println(Control.STOPPED);
                            break commands;
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
        System.out.println(title + " stopped");
    }
}
