package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import static java.util.concurrent.TimeUnit.SECONDS;

import io.lettuce.core.RedisClient;
import io.lettuce.core.codec.StringCodec;
import io.lettuce.core.output.StatusOutput;
import io.lettuce.core.protocol.CommandArgs;
import io.lettuce.core.protocol.CommandType;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadMissing;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.host.ZLinkFrameworkRuntimeBootstrap;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.testsupport.ZLinkHostServing;
import systems.zlink.framework.testsupport.ZLinkTestRedis;

import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;

final class ZLinkRedisLifecycleCompletionTest {
    @Test
    void blockedActorFindDependentLeavesRedisConnectionFree() throws Exception {
        String endpoint = ZLinkTestRedis.endpoint();
        try (var store =
                        new ZLinkRedisLocationStore(
                                new ZLinkRedisLocationOptions()
                                        .setConnectionString(endpoint)
                                        .setKeyPrefix(
                                                "zlink:lifecycle-completion:"
                                                        + UUID.randomUUID()));
                var controlClient = RedisClient.create(endpoint);
                var control = controlClient.connect()) {
            var options = new DefaultZLinkFrameworkOptions();
            options.addLocationStore(store);
            options.addRouteMesh("lifecycle").listen("tcp://127.0.0.1:0").objects().server();
            try (var runtime =
                    ZLinkFrameworkRuntimeBootstrap.start(
                            options,
                            new ZLinkJavaBackendAdapterFactory(),
                            ZLinkHandlerActivator.reflection(),
                            new ZLinkRuntimeEventDispatcher())) {
                ZLinkHostServing.await(runtime);
                var actors = runtime.actorManager();
                assertTrue(actors.find("warmup").toCompletableFuture().get(5, SECONDS).isEmpty());

                var dependentThread = new CompletableFuture<String>();
                var releaseDependent = new CountDownLatch(1);
                try {
                    // Hold the Redis response until the application has registered its dependent.
                    assertEquals("OK", control.sync().clientPause(5_000));
                    var dependent =
                            actors.find("missing")
                                    .thenAccept(
                                            result -> {
                                                dependentThread.complete(
                                                        Thread.currentThread().getName());
                                                try {
                                                    assertTrue(releaseDependent.await(10, SECONDS));
                                                } catch (InterruptedException failure) {
                                                    Thread.currentThread().interrupt();
                                                    throw new AssertionError(failure);
                                                }
                                                assertTrue(result.isEmpty());
                                            });
                    assertEquals(
                            "OK",
                            control.sync()
                                    .dispatch(
                                            CommandType.CLIENT,
                                            new StatusOutput<>(StringCodec.UTF8),
                                            new CommandArgs<>(StringCodec.UTF8).add("UNPAUSE")));

                    String thread = dependentThread.get(5, SECONDS);
                    var otherRead =
                            store.read(new ZLinkStoreKey("other-read"), () -> false)
                                    .toCompletableFuture();
                    assertAll(
                            () ->
                                    assertInstanceOf(
                                            ZLinkStoreReadMissing.class,
                                            assertDoesNotThrow(
                                                    () -> otherRead.get(2, SECONDS),
                                                    "same Redis connection must read while the dependent waits")),
                            () -> {
                                String name = thread.toLowerCase(Locale.ROOT);
                                assertFalse(
                                        name.startsWith("lettuce-") || name.contains("ioeventloop"),
                                        "dependent ran on " + thread);
                            });
                    releaseDependent.countDown();
                    dependent.toCompletableFuture().get(5, SECONDS);
                } finally {
                    releaseDependent.countDown();
                }
            }
        }
    }
}
