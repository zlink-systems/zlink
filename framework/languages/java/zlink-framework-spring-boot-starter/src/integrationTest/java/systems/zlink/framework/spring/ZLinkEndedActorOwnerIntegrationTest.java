package systems.zlink.framework.spring;

import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Assumptions;
import org.junit.jupiter.api.Test;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.context.annotation.Import;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadMissing;
import systems.zlink.framework.locationprovider.ZLinkStoreScanItem;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locations.redis.*;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.spots.*;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class ZLinkEndedActorOwnerIntegrationTest {
    @Configuration(proxyBeanMethods = false)
    @EnableZLinkFramework
    @Import(ZLinkFrameworkAutoConfiguration.class)
    public static class Config {
        @Bean
        public ZLinkFrameworkConfigurer framework() {
            return options -> {
                System.out.println(
                        "LEASE_TTL_MS=" + options.configureLocations().ownerLeaseTtl().toMillis());
                options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
                var node = options.addRouteMesh("probe");
                node.listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from(System.getProperty("probe.node")));
                node.objects()
                        .server()
                        .addSpotFactory("entry", Entry.class, f -> f.disableRelocation());
                node.objects()
                        .server()
                        .addActorFactory(
                                "player", Player.class, Factory.class, f -> f.disableRelocation());
            };
        }

        @Bean(destroyMethod = "close")
        public ZLinkRedisLocationStore store() {
            return new ZLinkRedisLocationStore(
                    new ZLinkRedisLocationOptions()
                            .setConnectionString(System.getProperty("probe.redis"))
                            .setKeyPrefix(System.getProperty("probe.prefix")));
        }
    }

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            System.out.println("FACTORY_INIT pid=" + ProcessHandle.current().pid());
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class Entry implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public Entry(ZLinkSpotContext context) {
            this.context = context;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    @Test
    void disabledFactoryRecreatesActorFromEndedOwnerRecordsAfterNormalProcessExit()
            throws Exception {
        String redis = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        Assumptions.assumeTrue(redis != null && !redis.isBlank());
        var logs = Files.createTempDirectory("zlink-1570-public-");
        System.out.println("Ended Actor owner logs: " + logs);
        String prefix = "actor-reclaim-" + UUID.randomUUID() + ":";
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(redis)
                                .setKeyPrefix(prefix))) {
            long previous = 0;
            for (int index = 0; index < 3; index++) {
                String mode = index == 1 ? "getOrCreate" : "create";
                var output = logs.resolve("node-" + index + ".log");
                var command =
                        new ArrayList<>(
                                List.of(
                                        Path.of(System.getProperty("java.home"), "bin", "java")
                                                .toString(),
                                        "--enable-native-access=ALL-UNNAMED",
                                        "-Dprobe.redis=" + redis,
                                        "-Dprobe.prefix=" + prefix,
                                        "-Dprobe.node=restarted-node",
                                        "-cp",
                                        System.getProperty("zlink.test.classpath"),
                                        getClass().getName(),
                                        mode));
                if (index < 2) command.add("hold");
                Process child = new ProcessBuilder(command).redirectErrorStream(true).start();
                var createdOutput = new CompletableFuture<String>();
                Thread outputReader =
                        new Thread(
                                () -> {
                                    var content = new StringBuilder();
                                    try (var reader = child.inputReader();
                                            var writer = Files.newBufferedWriter(output)) {
                                        String line;
                                        while ((line = reader.readLine()) != null) {
                                            writer.write(line);
                                            writer.newLine();
                                            writer.flush();
                                            content.append(line).append('\n');
                                            if (line.startsWith("CREATED generation="))
                                                createdOutput.complete(content.toString());
                                        }
                                        createdOutput.completeExceptionally(
                                                new AssertionError(
                                                        "Public Actor creation did not complete: "
                                                                + content));
                                    } catch (IOException failure) {
                                        createdOutput.completeExceptionally(failure);
                                    }
                                },
                                "ended-actor-owner-output");
                outputReader.start();
                List<ZLinkStoreScanItem> retained = List.of();
                ZLinkStoreKey ownerKey = null;
                try {
                    String content = createdOutput.get(60, TimeUnit.SECONDS);
                    Matcher created = Pattern.compile("CREATED generation=(\\d+)").matcher(content);
                    Assertions.assertTrue(created.find(), content);
                    long generation = Long.parseLong(created.group(1));
                    Matcher listener =
                            Pattern.compile("LISTEN_ENDPOINT=tcp://127\\.0\\.0\\.1:(\\d+)")
                                    .matcher(content);
                    Assertions.assertTrue(listener.find(), content);
                    Assertions.assertTrue(Integer.parseInt(listener.group(1)) > 0);
                    Assertions.assertTrue(generation > previous);
                    Assertions.assertTrue(content.contains("FACTORY_INIT pid=" + child.pid()));
                    previous = generation;
                    System.out.println(mode + " generation=" + generation);
                    if (index < 2) {
                        var page =
                                Assertions.assertInstanceOf(
                                        ZLinkStoreScanPageResult.class,
                                        store.scan(
                                                        new ZLinkStoreScanRequest(
                                                                "owner-lease\0", null, 2),
                                                        () -> false)
                                                .toCompletableFuture()
                                                .get());
                        Assertions.assertEquals(1, page.value().items().size());
                        ownerKey = page.value().items().get(0).key();
                        retained = ZLinkEndedOwnerStoreFixture.snapshot(store);
                        child.getOutputStream().close();
                        Assertions.assertTrue(child.waitFor(10, TimeUnit.SECONDS));
                    } else {
                        Assertions.assertTrue(child.waitFor(30, TimeUnit.SECONDS));
                        Assertions.assertEquals(0, child.exitValue());
                    }
                } finally {
                    try {
                        child.getOutputStream().close();
                    } finally {
                        try {
                            child.waitFor();
                        } finally {
                            outputReader.join();
                        }
                    }
                }
                Assertions.assertEquals(0, child.exitValue());
                if (index < 2) {
                    ZLinkEndedOwnerStoreFixture.restore(store, retained);
                    Assertions.assertInstanceOf(
                            ZLinkStoreReadMissing.class,
                            store.read(ownerKey, () -> false).toCompletableFuture().get());
                    System.out.println("OWNER_LEASE_ENDED node=" + index);
                }
            }
        }
    }

    public static void main(String[] args) throws Exception {
        try (var app =
                new SpringApplicationBuilder(Config.class).web(WebApplicationType.NONE).run()) {
            System.out.println(
                    "LISTEN_ENDPOINT="
                            + app.getBean(ZLinkFrameworkRuntime.class)
                                    .listenerStatus(ZLinkListenerKind.ROUTE_MESH, "probe")
                                    .endpoint());
            var actors = app.getBean(ZLinkActorManager.class);
            var result =
                    args[0].equals("create")
                            ? actors.create("reclaim-actor", "player")
                                    .inMesh("probe")
                                    .submit()
                                    .toCompletableFuture()
                                    .get()
                            : actors.getOrCreate("reclaim-actor", "player")
                                    .inMesh("probe")
                                    .submit()
                                    .toCompletableFuture()
                                    .get();
            if (!(result instanceof ZLinkActorCreateResult.Created created))
                throw new AssertionError("Expected Created, got " + result);
            System.out.println("CREATED generation=" + created.actor().objectGeneration());
            System.out.flush();
            if (args.length > 1) System.in.read();
        }
    }
}
