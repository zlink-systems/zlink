package systems.zlink.framework.spring;

import org.springframework.boot.WebApplicationType;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.context.annotation.Import;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.*;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

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
                node.listen("tcp://127.0.0.1:" + System.getProperty("probe.port"))
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

    @org.junit.jupiter.api.Test
    void disabledFactoryRecreatesActorAfterOwnerProcessIsKilled() throws Exception {
        String redis = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        org.junit.jupiter.api.Assumptions.assumeTrue(redis != null && !redis.isBlank());
        var logs = java.nio.file.Files.createTempDirectory("zlink-1570-public-");
        System.out.println("Actor crash logs: " + logs);
        String prefix = "actor-reclaim-" + java.util.UUID.randomUUID() + ":";
        int port;
        try (var listener = new java.net.ServerSocket(0)) {
            port = listener.getLocalPort();
        }
        long previous = 0;
        for (int index = 0; index < 3; index++) {
            String mode = index == 1 ? "getOrCreate" : "create";
            var output = logs.resolve("node-" + index + ".log");
            var command =
                    new java.util.ArrayList<>(
                            java.util.List.of(
                                    java.nio.file.Path.of(
                                                    System.getProperty("java.home"), "bin", "java")
                                            .toString(),
                                    "--enable-native-access=ALL-UNNAMED",
                                    "-Dprobe.redis=" + redis,
                                    "-Dprobe.prefix=" + prefix,
                                    "-Dprobe.node=restarted-node",
                                    "-Dprobe.port=" + port,
                                    "-cp",
                                    System.getProperty("zlink.test.classpath"),
                                    getClass().getName(),
                                    mode));
            if (index < 2) command.add("hold");
            Process child =
                    new ProcessBuilder(command)
                            .redirectErrorStream(true)
                            .redirectOutput(output.toFile())
                            .start();
            try {
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(60);
                String content;
                java.util.regex.Matcher created;
                while (true) {
                    content = java.nio.file.Files.readString(output);
                    created =
                            java.util.regex.Pattern.compile("CREATED generation=(\\d+)")
                                    .matcher(content);
                    if (created.find()) break;
                    org.junit.jupiter.api.Assertions.assertTrue(
                            child.isAlive() && System.nanoTime() < deadline,
                            () ->
                                    "Public Actor creation failed: "
                                            + output
                                            + "\n"
                                            + readOutput(output));
                    Thread.sleep(10);
                }
                long generation = Long.parseLong(created.group(1));
                org.junit.jupiter.api.Assertions.assertTrue(generation > previous);
                org.junit.jupiter.api.Assertions.assertTrue(
                        content.contains("FACTORY_INIT pid=" + child.pid()));
                previous = generation;
                System.out.println(mode + " generation=" + generation);
                if (index < 2) {
                    child.destroyForcibly();
                    org.junit.jupiter.api.Assertions.assertTrue(
                            child.waitFor(10, TimeUnit.SECONDS));
                    var ttl =
                            java.util.regex.Pattern.compile("LEASE_TTL_MS=(\\d+)").matcher(content);
                    org.junit.jupiter.api.Assertions.assertTrue(ttl.find());
                    Thread.sleep(Long.parseLong(ttl.group(1)));
                } else {
                    org.junit.jupiter.api.Assertions.assertTrue(
                            child.waitFor(30, TimeUnit.SECONDS));
                    org.junit.jupiter.api.Assertions.assertEquals(0, child.exitValue());
                }
            } finally {
                if (child.isAlive()) {
                    child.destroyForcibly();
                    child.waitFor();
                }
            }
        }
    }

    private static String readOutput(java.nio.file.Path path) {
        try {
            return java.nio.file.Files.readString(path);
        } catch (java.io.IOException failure) {
            throw new java.io.UncheckedIOException(failure);
        }
    }

    public static void main(String[] args) throws Exception {
        try (var app =
                new SpringApplicationBuilder(Config.class).web(WebApplicationType.NONE).run()) {
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
            if (args.length > 1) new CountDownLatch(1).await();
        }
    }
}
